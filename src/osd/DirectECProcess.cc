// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Copyright (C) 2013,2014 Cloudwatt <libre.licensing@cloudwatt.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */
#include "boost/tuple/tuple.hpp"
#include "PG.h"
#include "OSD.h"
#include "OpRequest.h"

#include "common/errno.h"
#include "common/perf_counters.h"

#include "json_spirit/json_spirit_reader.h"
#include "include/assert.h"  // json_spirit clobbers it
#include "include/rados/rados_types.hpp"

#ifdef WITH_LTTNG
#include "tracing/osd.h" // ??
#else
#define tracepoint(...)
#endif

#include <sstream>
#include <fstream>
#include <utility>

#include <errno.h>

#include "DirectECProcess.h"

#define dout_subsys ceph_subsys_directec
#undef dout_prefix
#define dout_prefix (*_dout << "ecop ")

static inline ostream& operator<<(ostream& os, pair<uint64_t, bufferlist>& outdata) {
  if (outdata.first == -1ULL)
    os << "(dne)";
  else
    os << "[" << outdata.first << ", " << outdata.second.length() << "]";
  return os;
}

void ECOp::dump(int level)
{
  hobject_t& soid = obc->obs.oi.soid;
  
  dout(level) << "oid: " << soid << dendl;
  if (repop)
    dout(level) << "repop[" << repop->rep_tid << "]" << dendl;
  else
    dout(level) << "repop[dne]" << dendl;

  dout(level) << "outdata: " << outdata << dendl;
  
  for (map<string, boost::optional<bufferlist> >::iterator i = map_xattr.begin();
       i != map_xattr.end(); ++i) {
    if (i->second)
      dout(level) << "map_xattr[" << i->first << "] set" << dendl;
    else
      dout(level) << "map_xattr[" << i->first << "] rm" << dendl;
  }

  dout(level) << "brmobj: " << brmobj << dendl;
  dout(level) << "brmop: " << brmop << dendl;
  dout(level) << "lasttid: " << lasttid << dendl;

  if (old_obs.exists)
    dout(level) << "old_obs: " << old_obs.oi.size << dendl;
  else
    dout(level) << "old_obs: (dne)" << dendl;

  if (next_op_obs.exists)
    dout(level) << "new_obs: " << next_op_obs.oi.size << dendl;
  else
    dout(level) << "new_obs: (dne)" << dendl;

  std::list<DirectECOp *>::iterator itr;
  if (list_clientop_writeread.size()) {
    dout(level) << "list_clientop_writeread size: " << list_clientop_writeread.size() << dendl;
    for (itr = list_clientop_writeread.begin(); itr != list_clientop_writeread.end(); ++itr)
      (*itr)->dump(level);
  }
  
  if (list_clientop_write.size()) {
    dout(level) << "list_clientop_write size: " << list_clientop_write.size() << dendl;
    for (itr = list_clientop_write.begin(); itr != list_clientop_write.end(); ++itr)
      (*itr)->dump(level);
  }
}

void DirectECOp::dump(int level)
{
  dout(level) << "DirectECOp[" << rep_tid << "]" << dendl;
  for (vector<OSDOp>::iterator itr = repop->ctx->ops.begin(); itr != repop->ctx->ops.end(); ++itr)
    dout(level) << "  op[" << *itr << "] = " << itr->rval << dendl;

  ReplicatedPG::OpContext::OpsInfo& oi = repop->ctx->ops_info;
  dout(level) << "  ops_info.writefull: " << oi.writefull << dendl;
  dout(level) << "  ops_info.rmobj: " << oi.rmobj << dendl;
  if (oi.beginpos == -1ULL)
    dout(level) << "  modified_range: (dne)" << dendl;
  else
    dout(level) << "  modified_range: [" << oi.beginpos << ", " << oi.endpos << "]" << dendl;
}

static int check_offset_and_length(uint64_t offset, uint64_t length, uint64_t max)
{
  if (offset >= max ||
      length > max ||
      offset + length > max)
    return -EFBIG;

  return 0;
}

#undef dout_prefix
#define dout_prefix (*_dout << "ecpg[" << ecpg->info.pgid << "] ")

//函数功能: 合并CEPH_OSD_OP_WRITE,ZERO操作到内存中.
//参数:   [in]OSDOp &osd_op
//	[in,out]pair<uint64_t,bufferlist> &outdata
//        [in]bool bwritezero
//返回值: void
void MulOpProcess::apply_write_to_cache(OSDOp &osd_op,pair<uint64_t,bufferlist> &outdata,bool bwritezero){
  ceph_osd_op &op = osd_op.op;
  
  dout(10) << __func__ << " offset " << op.extent.offset << " length " << op.extent.length
       << " outdata " << outdata << dendl;
  
  bufferlist bufData;
  if(!bwritezero){
    if(0 == osd_op.indata.length())//for bug-offset+zero
      return;
    bufData.substr_of(osd_op.indata,0,osd_op.indata.length());
  }else{
    bufData.append_zero(op.extent.length);
  }

  if(-1ull == outdata.first){//内容为空时,直接赋值
    outdata.first = op.extent.offset;
    outdata.second.append(bufData);
  }else{//内容不为空时
    uint64_t isize = outdata.first + outdata.second.length();
    if(isize < op.extent.offset){//|_______| |....|
      bufferlist bufzero;
      bufzero.append_zero(op.extent.offset-isize);//补齐零
      outdata.second.append(bufzero);
      outdata.second.append(bufData);//追加到最后
    }else if(isize == op.extent.offset){//|_______|....|
      outdata.second.append(bufData);//直接追加
    }else if(op.extent.offset+op.extent.length > isize && 
        op.extent.offset < isize && outdata.first <= op.extent.offset ){//|_____________...|.....
      bufferlist buf;
      buf.substr_of(outdata.second,0,op.extent.offset-outdata.first);
      outdata.second.claim(buf);
      outdata.second.append(bufData);
    }else if(op.extent.offset+op.extent.length <= isize && 
        outdata.first <= op.extent.offset){//|____........___|
      bufferlist bufstart,bufend;
      bufstart.substr_of(outdata.second,0,op.extent.offset-outdata.first);
      bufend.substr_of(outdata.second,op.extent.offset+op.extent.length-outdata.first,isize-op.extent.offset-op.extent.length);
      outdata.second.claim(bufstart);
      outdata.second.append(bufData);
      outdata.second.append(bufend);
    }else{//op.extent.offset小于outdata.first情况
      if(op.extent.offset <= outdata.first &&
          outdata.first <= op.extent.offset+op.extent.length &&
          op.extent.offset+op.extent.length < isize){//|.........|....__________|
        bufferlist bufend;
        bufend.substr_of(outdata.second,op.extent.offset+op.extent.length-outdata.first,isize-op.extent.offset-op.extent.length);
        outdata.second.claim(bufData);
        outdata.second.append(bufend);
      }else if(op.extent.offset <= outdata.first &&
          isize <= op.extent.offset+op.extent.length ){//|..............|_______|.....|
        outdata.second.claim(bufData);
      }else if(op.extent.offset+op.extent.length <= outdata.first){//|...........| |_____|
        bufferlist buf;
        buf.append_zero(outdata.first-op.extent.offset-op.extent.length);
        bufData.append(buf);
        bufData.append(outdata.second);
        outdata.second.claim(bufData);
      }
      outdata.first = op.extent.offset;
    }
  }
}

//函数功能: 合并CEPH_OSD_OP_TRUNCATE操作到内存中.
//参数:   [in]OSDOp &osd_op
//	[in,out]pair<uint64_t,bufferlist> &outdata
//        [in]bool bwritezero
//        [in] int ioldlength
//返回值: void
void MulOpProcess::apply_truncate_to_cache(OSDOp &osd_op,pair<uint64_t,bufferlist> &outdata,int ioldlength){
  ceph_osd_op &op = osd_op.op;
  dout(10) << __func__ << " offset " << op.extent.offset << " outdata " << outdata << dendl;

  if(op.extent.offset == 0) {
    outdata.first = 0;
    outdata.second.clear();
    return;
  }
  uint64_t itruncatesize = op.extent.offset;
  int istripe_truncatesize = itruncatesize - (itruncatesize % stripe_width);
  if(ioldlength > istripe_truncatesize && outdata.first == -1ull)
    return;
  if(outdata.first == -1ull || //|____| ...
      (op.extent.offset < outdata.first && outdata.first != (-1ULL) )){//|...|___|
    int imargin = ioldlength%stripe_width;
    if(imargin == 0){
      outdata.first = ioldlength;
      bufferlist buf;
      buf.append_zero(op.extent.offset - ioldlength);
      outdata.second.claim(buf);
    }else{
      imargin = ioldlength - imargin + stripe_width;
      outdata.first = imargin;
      bufferlist buf;
      buf.append_zero(op.extent.offset - imargin);
      outdata.second.claim(buf);
    }    
  }else if(op.extent.offset < outdata.first + outdata.second.length() && 
      outdata.first <= op.extent.offset){//|_____________...|
      bufferlist buf;
      buf.substr_of(outdata.second,0,op.extent.offset-outdata.first);
      outdata.second.claim(buf);      
  }else if(outdata.first + outdata.second.length() < op.extent.offset){//|_______| |
    outdata.second.append_zero(op.extent.offset-outdata.first - outdata.second.length());//补齐零
  }else{//当op.extent.truncate_size == isize什么也不做
  }
}


//函数功能: 将DirectECOp应用到outdata上,并记录属性.
//参数:   [in,out]pair<uint64_t,bufferlist> &outdata
//	[in,out]map<std::string,boost::optional<bufferlist>> &xattr
//        [int]DirectECOp * directecop;
//	  [in,out]ECOpRef ecop
//返回值: 0,成功
//        -1,异常
int MulOpProcess::apply_ops_to_cache(pair<uint64_t,bufferlist> & outdata,map<string,boost::optional<bufferlist> > &xattr,
    DirectECOp * directecop,ECOpRef ecop){
  int iResult = 0;
  vector<OSDOp>& ops = directecop->repop->ctx->ops;
  dout(10) << __func__ << " ops " << ops << " outdata " << outdata << dendl;

  for(vector<OSDOp>::iterator p = ops.begin(); p != ops.end();++p) {
    OSDOp &osd_op = *p;

    ceph_osd_op &op = osd_op.op;
    bufferlist::iterator bp = osd_op.indata.begin();
    
    if(osd_op.rval < 0){//先处理write中隐含的truncate,跳过该osd_op执行下面的
      if(op.op == CEPH_OSD_OP_WRITE){
        if(op.extent.truncate_seq != 0 ){
          if(outdata.first != -1ull ){
            if(op.extent.truncate_size != outdata.first + outdata.second.length()){
              apply_truncate_to_cache(osd_op,outdata,ecop->old_obs.oi.size);
              ecop->brmobj = false;
            }
          }
          else{
            apply_truncate_to_cache(osd_op,outdata,ecop->old_obs.oi.size);
            ecop->brmobj = false;
          }
        }
      }
      continue;
    }

    ecop->brmobj = false;
    dout(10) << "apply_op_to_cache " << osd_op << dendl;
    switch(op.op){
      case CEPH_OSD_OP_WRITE://是否需要判断op.extent.offset在内存中的位置,大于,小于outdata.first//TODO
        if(op.extent.truncate_seq != 0 ){
          if(outdata.first != -1ull ){
            if(op.extent.truncate_size != outdata.first + outdata.second.length()){
              apply_truncate_to_cache(osd_op,outdata,ecop->old_obs.oi.size);
            }
          }else{
            apply_truncate_to_cache(osd_op,outdata,ecop->old_obs.oi.size);
          }
        }
        apply_write_to_cache(osd_op,outdata);
        break;
      case CEPH_OSD_OP_WRITEFULL:	  	
        outdata.first = 0;
        outdata.second.clear();


        if(op.extent.offset > 0){//offset 大于零时 ,前面需要补齐零.
          outdata.second.append_zero(op.extent.offset);
        }
        outdata.second.append(osd_op.indata);//数据

        break;
      case CEPH_OSD_OP_ZERO:
        apply_write_to_cache(osd_op,outdata,true);
        break;	  
      case CEPH_OSD_OP_TRIMTRUNC:	        		
      case CEPH_OSD_OP_TRUNCATE:
        apply_truncate_to_cache(osd_op,outdata,ecop->old_obs.oi.size);
        break;	  
      case CEPH_OSD_OP_SETXATTR:
        {
          string aname;
          bufferlist buf;

          bp.copy(op.xattr.name_len, aname);
          bp.copy(op.xattr.value_len, buf);
          string name = "_" + aname;

          xattr[name] = buf;	
        }
        break;
      case CEPH_OSD_OP_RMXATTR:
        {
          string aname;		
          bp.copy(op.xattr.name_len, aname);                
          string name = "_" + aname;                

          xattr[name] = boost::optional<bufferlist>();	
        }
        break;
      case CEPH_OSD_OP_APPEND:
        outdata.second.append(osd_op.indata);
        break;
      case CEPH_OSD_OP_CREATE:
        if(ecop->old_obs.exists || xattr.size() > 0 || outdata.first != (-1ULL))
          break;
        outdata.first = 0;
        outdata.second.clear();

        break;
      case CEPH_OSD_OP_DELETE:	  	
        xattr.clear();
        outdata.first = -1ull;
        outdata.second.clear();

        ecop->brmobj = true;
        ecop->brmop = true;

        break;

      case CEPH_OSD_OP_STARTSYNC:
        dout(0) << __func__ << " ops " << ops << " have STARTSYNC" << dendl;
        break;
      
      default:
        iResult = -EOPNOTSUPP;
        break;
    }
  }

  dout(10) << __func__ << " finished, outdata: " << outdata << dendl;
  ecop->next_op_obs = directecop->repop->ctx->new_obs;
  ecop->lasttid = directecop->rep_tid;
  return iResult;
}

//函数功能: 合并操作,根据type的类型进行不同列表的处理,1 == type时,ECWrite_Read操作读取数据完成后调用本函数,合并
//         操作,并将ECWrite_Read列表中的操作转移到ECWrite列表中;2 == type时,对齐的Append或者delete或者setxattr操作
//         完成后,处理后续的操作如果还是对齐的Append,delete,setxattr其中之一,直接发送对齐的Append,delete,setxattr操作
//         如果不是其中之一,则发送Write_Read操作;3==type时,如果有后续操作合并操作发送Write操作,如果没有后续操作则返回0
//参数:   [in]int type (0，ECWrite_Read；1，对齐的Append,delete,setxattr，2，ECWrite)
//	[in,out]ECOpRef ecop
//返回值: 0, 无后续操作
//        1, 发送Write_Read操作；
//        2，发送Write操作；
//        3，发送Append操作；
int MulOpProcess::merge_directecop(int type,ECOpRef ecop){
  dout(10) << __func__ << "(" << type << ", " << ecop->obc->obs.oi.soid << ")" << dendl;
  
  int iResult = DIRECT_EC_NO_OP;
  pair<uint64_t,bufferlist> &data = ecop->outdata; 
  map<string,boost::optional<bufferlist> > &xattr = ecop->map_xattr;
  if(MERGE_OP_WRITEREAD == type ){//0:处理 ECWrite_Read列表;
    for(std::list<DirectECOp *>::iterator iter = ecop->list_clientop_writeread.begin();
        iter != ecop->list_clientop_writeread.end();){
      //应用到内存中,属性
      apply_ops_to_cache(data,xattr,*iter,ecop);
      //将iter添加到write列表中,再从ECWrite_Read列表中删除
      ecop->list_clientop_write.push_back(*iter);
      ecop->list_clientop_writeread.erase(iter++);       

      iResult = DIRECT_EC_WRITEFULL_OP;
    }
  }else if(MERGE_OP_APPEND == type){//1:处理  ECWrite_Read列表,对齐的Append,delete,setxattr操作//TODO//
    unsigned int inumber = 0;
    
    for(std::list<DirectECOp *>::iterator iter = ecop->list_clientop_writeread.begin();
        iter != ecop->list_clientop_writeread.end();++iter){
      iResult = judge_op_type((*iter)->repop,ecop);
      if(DIRECT_EC_WRITEREAD_OP == iResult){//读取操作        
        break;
      }else if(DIRECT_EC_DELETE_OP == iResult || DIRECT_EC_WRITEFULL_OP == iResult){//delete,writefull为true时需要合并操作
        apply_ops_to_cache(data,xattr,*iter,ecop);
	
	    if(0 == inumber){//writefull操作,outdata数据需要从零开始,
          int imargin = ecop->outdata.first;
	      if(imargin > 0){
            bufferlist buf;
            buf.append_zero(imargin);
            buf.append(ecop->outdata.second);
            ecop->outdata.second.claim(buf);
            ecop->outdata.first -= imargin;
          }
        }
      }else if(DIRECT_EC_APPEND_OP == iResult){//对齐append,setxattr等
        apply_ops_to_cache(data,xattr,*iter,ecop);
      }
      ++inumber;
    }
    if(DIRECT_EC_APPEND_OP  == iResult && ecop->outdata.first != -1ull){//Append操作对齐处理
      uint64_t imargin = ecop->outdata.first - ecop->old_obs.oi.size ;
      
      if(imargin > 0){
        bufferlist buf;
        buf.append_zero(imargin);
        buf.append(ecop->outdata.second);
        ecop->outdata.second.claim(buf);
        ecop->outdata.first -= imargin;
      }
    }
    if(inumber == ecop->list_clientop_writeread.size()){
      if(DIRECT_EC_WRITEFULL_OP == iResult || DIRECT_EC_DELETE_OP == iResult){//writefull操作,需要将队列从list_clientop_writeread中转移到list_clientop_write中
        for(std::list<DirectECOp *>::iterator iter = ecop->list_clientop_writeread.begin();
            iter != ecop->list_clientop_writeread.end();){     
          ecop->list_clientop_write.push_back(*iter);
          ecop->list_clientop_writeread.erase(iter++);
        }
      }
    }    
  }else if(MERGE_OP_WRITE == type){//处理 ECWrite列表

    //2015-09-07 flc 添加 write列表中的操作是否需要writefull操作-处理对齐Append , 属性修改等操作数据重复写入的问题/////////////////
    if(ecop->list_clientop_write.size() && ecop->old_obs.oi.size != 0){
      ecop->is_appendop_in_write_list = true;
    } 
    
    for(std::list<DirectECOp *>::iterator iter = ecop->list_clientop_write.begin();
        iter != ecop->list_clientop_write.end();++iter){ 

      //2015-09-07 flc 判断操作是否会影响到数据/////////
      if(!is_appendop_in_writelist((*iter)->repop,ecop)){
        ecop->is_appendop_in_write_list = false;	
      }
      
      //应用到内存中,属性
      apply_ops_to_cache(data,xattr,*iter,ecop);
      iResult = DIRECT_EC_WRITEFULL_OP;
    }
//2015-09-28//flc 添加 write队列merge后判断outdata.first大于0时 补齐前面的内容//begin/////////////////
    if(ecop->outdata.first != -1ull){
      if(ecop->outdata.first > 0){
        uint64_t imargin = ecop->outdata.first;
      
        if(imargin > 0){
          bufferlist buf;
          buf.append_zero(imargin);
          buf.append(ecop->outdata.second);
          ecop->outdata.second.claim(buf);
          ecop->outdata.first -= imargin;
        }
      }
    }
//2015-09-28//flc 添加 write队列merge后判断outdata.first大于0时 补齐前面的内容//end///////////////////    
  }

  return iResult;
}

//函数功能:将当前操作添加到map_oid_ECOp中
//参数:   [in]RepGather* repop
//	[in]Context *on_locall_applied_sync
//	[in]Context *on_all_applied
//	[in]Context *on_all_commit
//	[in,out]ECOpRef& ecop
//返回值:  0, 等待处理
//         1, 发送Write_Read操作；
//	 2，发送Write操作；
//	 3，发送Append操作；
//	 4，发送delete操作；
int MulOpProcess::add_op_to_map(ReplicatedPG::RepGather* repop,
    Context *on_locall_applied_sync,
    Context *on_all_applied,
    Context *on_all_commit,
    ECOpRef& ecop)
{
  int iResult = DIRECT_EC_WAITING;
  //1)获取oid,以及ecop的赋值
  hobject_t oid = repop->ctx->obs->oi.soid;
  ecop = get_ecop(oid);
  if(NULL == ecop.get()){
    ECOpRef a(new ECOp);
    ecop = a;   
    map_oid_ECOp.insert(pair<hobject_t,ECOpRef>(oid, ecop));
    ecop->obc = repop->obc;//对象上下文赋值
  }

  DirectECOp * pdirectecop = new DirectECOp(on_locall_applied_sync,on_all_applied,on_all_commit,repop);  

  //2)遍历repop->ctx->ops//确定是什么操作//TODO//////////////////
  if(ecop->list_clientop_writeread.size()){//ECWrite_read队列不为空,添加到尾部
    ecop->list_clientop_writeread.push_back(pdirectecop);    
    return iResult;
  }else if(ecop->list_clientop_write.size()){//ECWrite队列不为空,添加到尾部
    ecop->list_clientop_write.push_back(pdirectecop);    
    return iResult;
  }else{//两个队列都为空时,遍历repop->ctx->ops判断op应该入什么队列.
    dout(10) << __func__ << " " << oid << dendl;
    ecop->old_obs = *(repop->ctx->obs);
    iResult = judge_op_type(repop,ecop);
    dout(10) << "judge_op_type(" << repop->rep_tid << ") = " << iResult << dendl;
    if(DIRECT_EC_WRITEREAD_OP == iResult){//读取操作
      ecop->list_clientop_writeread.push_back(pdirectecop);      
    }else if(DIRECT_EC_WRITEFULL_OP == iResult || DIRECT_EC_DELETE_OP == iResult){//delete ,writefull为true时需要合并操作
      ecop->list_clientop_write.push_back(pdirectecop);
      apply_ops_to_cache(ecop->outdata,ecop->map_xattr,pdirectecop,ecop);
    }else if(DIRECT_EC_APPEND_OP  == iResult){//对齐append,setxattr等
      ecop->list_clientop_writeread.push_back(pdirectecop);
      apply_ops_to_cache(ecop->outdata,ecop->map_xattr,pdirectecop,ecop);      
      
    }

    if((DIRECT_EC_APPEND_OP  == iResult || DIRECT_EC_WRITEFULL_OP  == iResult || DIRECT_EC_DELETE_OP == iResult)&& ecop->outdata.first != -1ull){
      uint64_t imargin = 0;
      if(DIRECT_EC_APPEND_OP == iResult){
        imargin = ecop->outdata.first - ecop->old_obs.oi.size ;	
      }else if(DIRECT_EC_WRITEFULL_OP  == iResult || DIRECT_EC_DELETE_OP == iResult){//writefull,delete 从0开始
        imargin = ecop->outdata.first;
	    iResult = DIRECT_EC_WRITEFULL_OP;
      }
      
      if(imargin > 0){
        bufferlist buf;
        buf.append_zero(imargin);
        buf.append(ecop->outdata.second);
        ecop->outdata.second.claim(buf);
        ecop->outdata.first  -= imargin;
      }
    }
  }
  
  return iResult;
}

//函数功能:ECWrite_Read操作读取数据完成后；将list_clientop_writeread列表中的操作应用到读取的数据中,并将操作从列表
//         list_clientop_writeread中转移到list_clientop_write列表中,并标志该操作已经应用.
//参数:[in,out]ECOpRef ecop
//返回值:void
void MulOpProcess::eval_writeread(ECOpRef ecop)
{
  dout(10) << __func__ << " " << ecop->obc->obs.oi.soid << dendl;
  //1)遍历list_clientop_writeread中的op，将操作应用到内存中；
  //2)将操作转移到write列表中；并标识当前操作已经应用。
  merge_directecop(MERGE_OP_WRITEREAD,ecop);
}
//函数功能:处理返回的write操作；会把所有已经处理的write操作，执行applid、commit操作；
//         如果状态是applid且commit的，那么把该任务从任务队列中删除；
//参数:   [in]int type (1，applid；2，commit，3，local_sync)
//	  [in,out]ECOpRef ecop
//返回值: 0，不处理；
//	!0，继续Write；
int MulOpProcess::eval_write(int type,ECOpRef ecop){
  dout(10) << __func__ << "(" << type << ", " << ecop->obc->obs.oi.soid 
       << " " << ecop->lasttid << ")" << dendl;
  
  int iResult = DIRECT_EC_NO_OP;
  //1)遍历list_clientop_write中的op，应用过的op，调用pg层传送过来的回调函数返回给客户端，
  if(DIRECT_EC_OP_APPLIED == type)//applid
  {
    for(std::list<DirectECOp *>::iterator iter = ecop->list_clientop_write.begin();
        iter != ecop->list_clientop_write.end();++iter){
      //判断该操作是否应用
      if(ecop->lasttid >= (*iter)->rep_tid){
        //修改iter->repop//TODO//
        //执行回调
        (*iter)->on_all_applied->complete(0);
        (*iter)->on_all_applied = 0;
      }
    }
    ecop->repop->all_applied = true;
  }
  else if(DIRECT_EC_OP_COMMITTED == type)//commit
  {
    for(std::list<DirectECOp *>::iterator iter = ecop->list_clientop_write.begin();
        iter != ecop->list_clientop_write.end();++iter){
      //判断该操作是否应用
      if(ecop->lasttid >= (*iter)->rep_tid){
        //执行回调
        (*iter)->on_all_commit->complete(0);
        (*iter)->on_all_commit = 0;
      }
    }

    ecop->repop->all_committed = true;
  }
  else if(DIRECT_EC_OP_LOCALSYNC == type)//on_local_applied_sync
  {
    for(std::list<DirectECOp *>::iterator iter = ecop->list_clientop_write.begin();
        iter != ecop->list_clientop_write.end();++iter){
      //判断该操作是否应用
      if(ecop->lasttid >= (*iter)->rep_tid){           
        (*iter)->on_local_applied_sync->complete(0);
        (*iter)->on_local_applied_sync = 0;	   
      }
    }       
  }

  

  //2)未应用过的op,应用到内存中，并标记应用的位置
  if(ecop->repop->all_applied && ecop->repop->all_committed){
    ecop->map_xattr.clear();//清空本次操作属性
    //删除应用完成的操作
    for(std::list<DirectECOp *>::iterator iter = ecop->list_clientop_write.begin();
      iter != ecop->list_clientop_write.end();){
      //判断该操作是否应用
      if(ecop->lasttid >= (*iter)->rep_tid){
        delete *iter;
        ecop->list_clientop_write.erase(iter++);
      }else{
        break;
      }
    }
    if(ecop->list_clientop_write.size() > 0){
      iResult = merge_directecop(MERGE_OP_WRITE,ecop);
    }else{
      return DIRECT_EC_NO_OP;
    }
    
  }
  return iResult;

}

//函数功能:处理返回的Append操作；会把所有已经处理的Append操作，执行applid、commit操作；
//         如果状态是applid且commit的，那么把该任务从任务队列中删除；
//参数:     [in]int type (1，applid；2，commit，3，local_sync)
//	  [in,out]ECOpRef ecop
//返回值: 0，不处理；
//        1, 发送Write_Read操作；
//        2，发送Write操作；
//        3，发送Append操作；
int MulOpProcess::eval_append(int type,ECOpRef ecop){
  dout(10) << __func__ << "(" << type << ", " << ecop->obc->obs.oi.soid 
       << " " << ecop->lasttid << ")" << dendl;
  
  int iResult = DIRECT_EC_NO_OP;
  if(DIRECT_EC_OP_APPLIED == type)//applid
  {
    for(std::list<DirectECOp *>::iterator iter = ecop->list_clientop_writeread.begin();
        iter != ecop->list_clientop_writeread.end();++iter){
      //判断该操作是否应用
      if(ecop->lasttid >= (*iter)->rep_tid){
        //修改iter->repop//TODO//
        //执行回调
        (*iter)->on_all_applied->complete(0);
        (*iter)->on_all_applied = 0;
      }
    }
    ecop->repop->all_applied = true;
  }
  else if(DIRECT_EC_OP_COMMITTED == type)//commit
  {
    for(std::list<DirectECOp *>::iterator iter = ecop->list_clientop_writeread.begin();
        iter != ecop->list_clientop_writeread.end();++iter){
      //判断该操作是否应用
      if(ecop->lasttid >= (*iter)->rep_tid){
        //修改iter->repop//TODO//
        //执行回调
        (*iter)->on_all_commit->complete(0);
        (*iter)->on_all_commit = 0;
      }
    }
    ecop->repop->all_committed = true;
  }
  else if(DIRECT_EC_OP_LOCALSYNC == type)//on_local_applied_sync
  {
    for(std::list<DirectECOp *>::iterator iter = ecop->list_clientop_writeread.begin();
        iter != ecop->list_clientop_writeread.end();++iter){
      //判断该操作是否应用
      if(ecop->lasttid >= (*iter)->rep_tid){           
        (*iter)->on_local_applied_sync->complete(0);
        (*iter)->on_local_applied_sync = 0;	   
      }
    }       
  }
  
  //2)未应用过的op,应用到内存中，并标记应用的位置。
  if(ecop->repop->all_applied && ecop->repop->all_committed){
    ecop->outdata.first = -1ull;//append操作需要清空
    ecop->outdata.second.clear();
    ecop->map_xattr.clear();

    //删除应用完成的操作
    for(std::list<DirectECOp *>::iterator iter = ecop->list_clientop_writeread.begin();
      iter != ecop->list_clientop_writeread.end();){
      //判断该操作是否应用
      if(ecop->lasttid >= (*iter)->rep_tid){
        delete *iter;
        ecop->list_clientop_writeread.erase(iter++);
      }else{
        break;
      }
    }

    if(ecop->list_clientop_writeread.size() > 0){
      iResult = merge_directecop(MERGE_OP_APPEND,ecop);
    }else{
      return DIRECT_EC_NO_OP;
    }
  }
  return iResult;

}
//函数功能:从map_oid_ECOp中获取key值为oid所对应的Value值 ECOp智能指针.
//参数:   [in]hobject_t oid
//返回值: NULL,  map_oid_ECOp中没有oid所对应的指针
//        ECOpRef，oid所对应的ECOp指针
ECOpRef  MulOpProcess::get_ecop(hobject_t oid){
  ECOpRef ecop;
  std::map<hobject_t,ECOpRef>::iterator iter;
  iter = map_oid_ECOp.find(oid);
  if(iter != map_oid_ECOp.end()){
    ecop = iter->second;
  }
  
  return ecop;
}

void MulOpProcess::set_version(ECOpRef ecop, object_stat_collection_t& delta)
{
  ReplicatedPG::OpContext* ctx = ecop->repop->ctx;
  eversion_t& version = ctx->at_version;
  version_t user_version = ctx->user_at_version;
  std::list<DirectECOp*>* ops = &(ecop->list_clientop_writeread);
  if (ops->empty())
    ops = &(ecop->list_clientop_write);

  for (std::list<DirectECOp*>::iterator itr = ops->begin(); itr != ops->end(); ++itr) {
    if ((*itr)->rep_tid <= ecop->lasttid) {
      ReplicatedPG::OpContext* cur = (*itr)->repop->ctx;
      (*itr)->repop->v = version;
      cur->at_version = version;
      cur->user_at_version = user_version;
      ctx->mtime = cur->mtime;
      delta.add(cur->delta_stats, cur->obs->oi.category);
    } else {
      break;
    }
  }
}

//函数功能: 清空map_oid_ECOp,释放map中的资源.
//参数:   void
//返回值: void
void MulOpProcess::on_change(){
  map_oid_ECOp.clear();      
}
//函数功能: 从map_oid_ECOp中删除特定的ecop.
//参数: [in]ECOpRef ecop
//返回值: void
void MulOpProcess::delete_ecop_from_map(ECOpRef ecop){
  hobject_t soid = ecop->repop->ctx->obs->oi.soid; 
  map<hobject_t,ECOpRef>::iterator iter = map_oid_ECOp.find(soid);
  if(map_oid_ECOp.end() != iter){
    if(iter->second->list_clientop_writeread.size() == 0 &&
        iter->second->list_clientop_write.size() == 0){
      dout(10) << __func__ << " " << soid << dendl;
      map_oid_ECOp.erase(soid);
    }
  }
}

/*
 *ecop如果没有生成之前，就是在add_op_to_map中调用的时候，直接使用空的ecop就可以
 *
 *
 *Return: 1,WriteRead;2,WriteFull;3,Append;4,DELETE;
 */
int MulOpProcess::judge_op_type(ReplicatedPG::RepGather* repop,ECOpRef& ecop) {
  ReplicatedPG::OpContext *ctx = repop->ctx;
  ReplicatedPG::OpContext::OpsInfo &ops_info = ctx->ops_info;
  ObjectState& obs = ecop->old_obs;
  object_info_t& oi = obs.oi;
  pair<uint64_t,bufferlist> &outdata = ecop->outdata;


  
  if(ops_info.rmobj) //存在 delete 操作
    return DIRECT_EC_DELETE_OP;
  
  if(!obs.exists || ops_info.writefull) // create or write_full
    return DIRECT_EC_WRITEFULL_OP;

  if(ops_info.beginpos == ops_info.endpos){
    if(0 == oi.size || 0 == outdata.first){
      return DIRECT_EC_WRITEFULL_OP;
    }else{
      return DIRECT_EC_APPEND_OP;
    }
  }
  
  if(outdata.first == 0) // ?
    return DIRECT_EC_WRITEFULL_OP;

  uint64_t beginpos = ops_info.beginpos;
  uint64_t endpos = ops_info.endpos;

  if(outdata.first != (-1ULL) && outdata.second.length() > 0) {
    if(beginpos > outdata.first)
      beginpos = outdata.first;
    if(endpos < (outdata.first + outdata.second.length()))
      endpos = outdata.first + outdata.second.length();
  }

  if(beginpos == 0 && endpos >= oi.size)
    return DIRECT_EC_WRITEFULL_OP;

  beginpos -= (beginpos % stripe_width);
  if(beginpos >= oi.size && (oi.size % stripe_width == 0))
    return DIRECT_EC_APPEND_OP;

  return DIRECT_EC_WRITEREAD_OP; 
}

//2015-09-06日 flc 添加 //用于判断write列表中的操作是否是对齐Append操作函数//begin////////////////////// 
//函数功能: 判断操作是否是对齐的Append操作
//参数: [in]ReplicatedPG::RepGather* repop
//      [in]ECOpRef& ecop
//返回值: bool
//        ture:write列表中的操作是对齐的Append操作;
//        false:非对齐的Append操作;
bool MulOpProcess::is_appendop_in_writelist(ReplicatedPG::RepGather* repop,ECOpRef& ecop) {
  ReplicatedPG::OpContext *ctx = repop->ctx;
  ReplicatedPG::OpContext::OpsInfo &ops_info = ctx->ops_info;
  ObjectState& obs = ecop->old_obs;
  object_info_t& oi = obs.oi;
  pair<uint64_t,bufferlist> &outdata = ecop->outdata;
  
  uint64_t beginpos = ops_info.beginpos;
  uint64_t endpos = ops_info.endpos;

  if(outdata.first != (-1ULL) && outdata.second.length() > 0) {
    if(endpos < (outdata.first + outdata.second.length()))
      endpos = outdata.first + outdata.second.length();
  }

  dout(10) << __func__ << " beginpos & endpos & oi.size % stripe_width "<<beginpos<<" & "<<endpos
  	<<" & "<< oi.size % stripe_width << dendl;

  if(beginpos == 0 && endpos >= oi.size)
    return false;
  
  if(ops_info.beginpos == ops_info.endpos){//2015-09-08日//不影响数据的操作/////////
    if(0 != oi.size ){//2015-09-08日//是否可以去掉该条件//?
      return true;
    }
  }

  beginpos -= (beginpos % stripe_width);
  if(beginpos >= oi.size && (oi.size % stripe_width == 0)){//对齐的Append操作
    return true;
  }

  return false; 
}
//2015-09-06日 flc 添加 //用于判断write列表中的操作是否是对齐Append操作函数//end//////////////////////// 


//函数功能:ECPG在may_write||may_chache为真时，调用check_osd_ops。check_osd_ops主要有两个功能：
//        (1) 判断那些操作是不是应该交给DirectECProcess处理。(2)判断操作的参数有没有错误。
//参数:   [in]ReplicatedPG::OpContext * ctx
//返回值: int
//	  < 0：某个操作的参数有错误，上层应当结束处理并将该错误回复给客户端。
//	  = 0：DirectECProcess无法处理该请求，上层应当按原有流程处理。
//	  > 0：DirectECProcess需要处理该请求，上层应当调用DirectECProcess::do_osd_ops，进入直接纠删的处理流程。
int DirectECProcess::check_osd_ops(ReplicatedPG::OpContext* ctx) {
  int result = 0;
#if 0
  SnapSetContext* ssc = ctx->obc->ssc;
#endif
  ObjectState& obs = ctx->new_obs;
  object_info_t& oi = obs.oi;
  const hobject_t& soid = oi.soid;

  object_stat_sum_t& delta = ctx->delta_stats;
  uint64_t max_obj_size = ecpg->cct->_conf->osd_max_object_size;

  dout(10) << "check_osd_ops " << soid << " " << ctx->ops << dendl;
  
  for (vector<OSDOp>::iterator p = ctx->ops.begin(); 
      p != ctx->ops.end(); ++p, ctx->current_osd_subop_num++) {
    ceph_osd_op& op = p->op;
    bufferlist& indata = p->indata; // wumq
    bool failok = false; // wumq

    dout(10) << "check_osd_op " << *p << dendl;

    // munge -1 truncate to 0 truncate
    if (ceph_osd_op_uses_extent(op.op) &&
        op.extent.truncate_seq == 1 &&
        op.extent.truncate_size == (-1ULL)) {
      op.extent.truncate_size = 0;
      op.extent.truncate_seq = 0;
    }

    // munge ZERO -> TRUNCATE? (don't munge to DELETE or we risk hosing attributes)
    if (op.op == CEPH_OSD_OP_ZERO &&
        obs.exists &&
        op.extent.offset < max_obj_size &&
        op.extent.length >= 1 &&
        op.extent.length <= max_obj_size &&
        op.extent.offset + op.extent.length >= oi.size) {
      if (op.extent.offset >= oi.size) {
        result = -EINVAL;
        failok = true;
        goto fail;
      }
      dout(10) << "munging ZERO " << op.extent.offset << "~" << op.extent.length
        << " -> TRUNCATE " << op.extent.offset << " (old size is " << oi.size << ")" << dendl;
      op.op = CEPH_OSD_OP_TRUNCATE;
    }

    switch(op.op) {
      case CEPH_OSD_OP_APPEND:
        dout(20) << "munging APPEND -> WRITE " << oi.size 
          << ", truncate_seq " << oi.truncate_seq << ", truncate_size " << oi.truncate_size << dendl;
        op.op = CEPH_OSD_OP_WRITE;
        op.extent.offset = oi.size;
        op.extent.truncate_seq = oi.truncate_seq;
        // falling through

      case CEPH_OSD_OP_WRITE:
        {
          if (op.extent.length != indata.length()) {
            result = -EINVAL;
            break;
          }

          __u32 seq = oi.truncate_seq;
          if (seq > op.extent.truncate_seq) {
            // old write, arrived after trimtrunc
            dout(5) << "old truncate_seq " << op.extent.truncate_seq << " < current " << seq 
              << ", adjusting truncate size to " << oi.size << dendl;
            op.extent.truncate_seq = 0; // invalid it

            if (op.extent.offset + op.extent.length > oi.size) {
              op.extent.length = (op.extent.offset >= oi.size) ? 0 : (oi.size - op.extent.offset);
              dout(5) << "adjusting write length to " << op.extent.length << dendl;
              bufferlist t;
              t.substr_of(indata, 0, op.extent.length);
              indata.swap(t);
            }
          } else if (op.extent.truncate_seq > seq) {
            // write arrives before trimtrunc
            oi.truncate_seq = op.extent.truncate_seq;
            oi.truncate_size = op.extent.truncate_size;

            if (obs.exists && !oi.is_whiteout()) {
              dout(5) << "truncate_seq " << oi.truncate_seq << " > current " << seq
                << ", truncating to " << oi.truncate_size << dendl;
              if (oi.truncate_size != oi.size) {
                ++ctx->num_write;
                if (oi.truncate_size < oi.size)
                  ctx->ops_info.update(oi.truncate_size, oi.size);
                else 
                  ctx->ops_info.update(oi.size, oi.truncate_size);

                delta.num_bytes -= oi.size;
                oi.size = oi.truncate_size;
                delta.num_bytes += oi.size;
              }
            } else {
              dout(0) << "truncate_seq " << oi.truncate_seq << " > current " << seq
                << ", but object is new" << dendl;
              op.extent.truncate_seq = 0; // invalid it
            }
          } else if (seq) { // wumq
            dout(5) << "old truncate_seq " << op.extent.truncate_seq << " = current" 
              << ", old truncate_size " << op.extent.truncate_size 
              << ", truncate_size " << oi.truncate_size << dendl;
            op.extent.truncate_seq = 0; // invalid it
          }

          result = check_offset_and_length(op.extent.offset, op.extent.length, max_obj_size);
          if (result < 0)
            break;

          ++ctx->num_write;
          uint64_t endpos = op.extent.offset + op.extent.length;
          if (op.extent.length)
            ctx->ops_info.update(op.extent.offset, endpos);
          if (!obs.exists) {
            ++delta.num_objects;
            obs.exists = true;
          }

          // update size and usage
          interval_set<uint64_t> ch;
          if (op.extent.length) {
            ch.insert(op.extent.offset, op.extent.length);
            ctx->modified_ranges.union_of(ch);

            if (endpos > oi.size) {
              delta.num_bytes += endpos - oi.size;
              oi.size = endpos;
            }
          }
          delta.num_wr++;
          delta.num_wr_kb += SHIFT_ROUND_UP(op.extent.length, 10);
        }
        break;

      case CEPH_OSD_OP_WRITEFULL:
        {
          if (op.extent.length != indata.length()) {
            result = -EINVAL;
            break;
          }
          result = check_offset_and_length(op.extent.offset, op.extent.length, max_obj_size);
          if (result < 0)
            break;

          ++ctx->num_write;
          ctx->ops_info.writefull = true;
          uint64_t new_size(0);
          ctx->ops_info.invalid();
          if (op.extent.length) {
            new_size = op.extent.offset + op.extent.length;
            ctx->ops_info.update(op.extent.offset, new_size);
          }
          if (!obs.exists) {
            ++delta.num_objects;
            obs.exists = true;
          }

          // update size and usage
          if (oi.size > 0) {
            interval_set<uint64_t> ch;
            ch.insert(0, oi.size);
            ctx->modified_ranges.union_of(ch);
          }
          delta.num_bytes += new_size - oi.size;
          oi.size = new_size;
          delta.num_wr_kb += SHIFT_ROUND_UP(op.extent.length, 10);
          delta.num_wr++;
        }
        break;

      case CEPH_OSD_OP_ZERO:
        {
          result = check_offset_and_length(op.extent.offset, op.extent.length, max_obj_size);
          if (result < 0)
            break;

          if (!op.extent.length) {
            dout(5) << "ZERO length = 0, no assert, just return -EINVAL" << dendl;
            result = -EINVAL;
            break;
          }

          if (!obs.exists || oi.is_whiteout())
          {
            dout(5) << "object " << soid << " dne, ZERO is a no-op" << dendl;
            result = -ENOENT;
            failok = true;
            break;
          }

          ++ctx->num_write;
          ctx->ops_info.update(op.extent.offset, op.extent.offset + op.extent.length);

          interval_set<uint64_t> ch;
          ch.insert(op.extent.offset, op.extent.length);
          ctx->modified_ranges.union_of(ch);
          delta.num_wr++;
        }
        break;

      case CEPH_OSD_OP_CREATE:
        {
          int flags = le32_to_cpu(op.flags);
          if (obs.exists && !oi.is_whiteout() && 
              (flags & CEPH_OSD_OP_FLAG_EXCL)) {
            result = -EEXIST; /* this is an exclusive create */
            break;
          }

          if (indata.length()) {
            bufferlist::iterator p = indata.begin();
            string category;
            try {
              ::decode(category, p);
            } catch (buffer::error& e) {
              result = -EINVAL;
              goto fail;
            }
            if (category.size()) {
              if (obs.exists && !oi.is_whiteout()) {
                if (oi.category != category)
                  result = -EEXIST; // category cannot be reset
              } else {
                oi.category = category;
              }
            }
          }

          if (result >= 0) {
            ++ctx->num_write;
            if (!obs.exists) {
              delta.num_objects++;
              obs.exists = true;
            }
          }
        }
        break;

      case CEPH_OSD_OP_TRIMTRUNC:
        op.extent.offset = op.extent.truncate_size;
        // falling through

      case CEPH_OSD_OP_TRUNCATE:
        {
          if (!obs.exists || oi.is_whiteout()) {
            dout(5) << "object " << soid << " dne, TRUNCATE is a no-op" << dendl;
            result = -ENOENT;
            failok = true;
            break;
          }

          if (op.extent.offset > max_obj_size) {
            result = -EFBIG;
            break;
          }

          if (op.extent.truncate_seq) {
            assert(op.extent.offset == op.extent.truncate_size);
            if (op.extent.truncate_seq <= oi.truncate_seq) {
              dout(5) << "truncate seq " << op.extent.truncate_seq << " <= current " << oi.truncate_seq
                << ", no-op" << dendl;
              result = -ERANGE;
              failok = true;
              break;
            }
            dout(10) << "truncate seq " << op.extent.truncate_seq << " > current " << oi.truncate_seq
              << ", truncating" << dendl;
            oi.truncate_seq = op.extent.truncate_seq;
            oi.truncate_size = op.extent.truncate_size;
          }

          ++ctx->num_write;
          if (oi.size > op.extent.offset) {
            ctx->ops_info.update(op.extent.offset, oi.size);
            interval_set<uint64_t> trim;
            trim.insert(op.extent.offset, oi.size - op.extent.offset);
            ctx->modified_ranges.union_of(trim);
            delta.num_bytes -= oi.size - op.extent.offset;
          } else {
            ctx->ops_info.update(oi.size, op.extent.offset);
            delta.num_bytes += op.extent.offset - oi.size; 
          }
          oi.size = op.extent.offset;
          delta.num_wr++;
          // do not set exists, or we will break above DELETE -> TRUNCATE munging.
        }
        break;

      case CEPH_OSD_OP_DELETE:
        {
          if (!obs.exists || oi.is_whiteout()) {
            result = -ENOENT;
            break;
          }

          ++ctx->num_write;
          ctx->ops_info.rmobj = true;
          ctx->ops_info.invalid();
          if (oi.size > 0) {
            interval_set<uint64_t> ch;
            ch.insert(0, oi.size);
            ctx->modified_ranges.union_of(ch);
          }

#if 0
          if (soid.is_snap()) {
            assert(ssc->snapset.clone_overlap.count(soid.snap));
            delta.num_bytes -= ssc->snapset.get_clone_bytes(soid.snap);
          } else
#endif
            delta.num_bytes -= oi.size;
          oi.size = 0;
          --delta.num_objects;
          ++delta.num_wr;

#if 0
          if (soid.is_snap())
            --delta.num_object_clones;
          if (oi.is_whiteout()) {
            dout(20) << __func__ << " deleting whiteout on " << soid << dendl;
            --delta.num_whiteouts;
          }
          if (soid.is_head())
            ctx->new_snapset.head_exists = false;
#endif
          obs.exists = false;
        }
        break;

      case CEPH_OSD_OP_SETXATTR:
        {
          uint64_t max_attr_size = ecpg->cct->_conf->osd_max_attr_size;
          if (max_attr_size && op.xattr.value_len > max_attr_size) {
            result = -EFBIG;
            break;
          }

          uint64_t max_name_len = MIN(ecpg->osd->store->get_max_attr_name_length(),
              ecpg->cct->_conf->osd_max_attr_name_len);
          if (op.xattr.name_len > max_name_len) {
            result = -ENAMETOOLONG;
            break;
          }

          ++ctx->num_write;
          if (!obs.exists) {
            ++delta.num_objects;
            obs.exists = true;
          }
          ++delta.num_wr;
        }
        break;

      case CEPH_OSD_OP_RMXATTR:
        ++ctx->num_write;
        ++delta.num_wr;
        break;

      case CEPH_OSD_OP_STARTSYNC:
        break;

      default: // op not supportted by DirectECProcess
        return 0;
    }

fail:
    p->rval = result;
    if (result < 0 && ((op.flags & CEPH_OSD_OP_FLAG_FAILOK) || failok))
      result = 0;

    if (result < 0)
      return result;
  }

  ctx->user_modify = true;
  return 1;
}

//函数功能:ECPG在check_osd_ops()返回值大于0时，将调用该方法进入直接纠删的处理流程。
//         在调用之前，要构造repop对象，以及三个回调。
//参数: [in]RepGather* repop
//      [in]Context *on_locall_applied_sync
//      [in]Context *on_all_applied
//      [in]Context *on_all_commit
//返回值:void
void DirectECProcess::do_osd_ops(ReplicatedPG::RepGather* repop,
    Context *on_locall_applied_sync,
    Context *on_all_applied,
    Context *on_all_commit){
  int iReturn = 0;
  ECOpRef ecop;
  //1.将repop添加到map中,并判断是什么操作,并应用到内存中,(在add_op_to_map中会记录对象操作前的状态,在合并后生成事务时用到)
  iReturn = pMulOpPro->add_op_to_map(repop,on_locall_applied_sync,on_all_applied,on_all_commit,ecop);
  //2.将repop中的new_obs更新到内存obc中
  repop->ctx->obc->obs = repop->ctx->new_obs;

  if(iReturn == 0){//map队列不为空,等待
    return ;
  }else if (1 == iReturn){//发送ECWrite_Read操作
    start_async_ecwrite_reads(ecop->old_obs,ecop); 
    return;
  }//后续 writefull,append,setxattr,delete等操作

  //3.处理ECOp中的outdata、map_xattr内容到Repop中；并生成Transaction及相关log；对象的初始状态ecop->old_obs
  iReturn = process_new_op(ecop,iReturn);
  //4.提交事务
  if(0 != iReturn){
    issue_repop(ecop);
  }else {
    pMulOpPro->eval_write(DIRECT_EC_OP_LOCALSYNC,ecop);
    pMulOpPro->eval_write(DIRECT_EC_OP_COMMITTED,ecop);
    pMulOpPro->eval_write(DIRECT_EC_OP_APPLIED,ecop);
    pMulOpPro->delete_ecop_from_map(ecop);
    return;
  }
}

//ECWrite_Read操作的开始函数，
//函数功能: 发送ECWrite_Read操作,根据opcontext获取对象的信息(对象的大小),读取的数据存储在ecOp->outdata中.
//参数:   [in]ObjectState& obs
//	[in,out]ECOp * ecop
//返回值: void
void DirectECProcess::start_async_ecwrite_reads(ObjectState& obs,ECOpRef ecop){
  //1)获取对象信息
  object_info_t &oi = obs.oi;
  const hobject_t &soid = oi.soid;

  uint64_t uoffset = 0;
  ecop->outdata.first = uoffset;
  //2)pending_async_reads添加读取的范围信息
  list < pair < pair<uint64_t, uint64_t>,
       pair<bufferlist*, Context*> > > pendingasyncreads;
  pendingasyncreads.push_back(
      make_pair(
        make_pair(uoffset,oi.size),
        make_pair(&(ecop->outdata.second),new WriteReadFillInExtent())));//new ECFillInExtent()

  dout(10) << __func__ << " " << soid << " size " << oi.size << dendl; 
  pgbackend->objects_read_async(soid,pendingasyncreads,new WriteReadContext(this, ecop));
  pendingasyncreads.clear();  	
}

//ECWrite_Read操作完成后的回调函数的处理函数。
//函数功能: ECWrite_Read操作的回调实体函数,将ECWrite_Read列表中的op应用到outdat中,并将op从ECWrite_Read列表转移到ECWrite
//	     列表中,记录已经应用的op.
//参数:  [in,out]ECOpRef ecop
//	
//返回值: void
void DirectECProcess::sub_write_read_reply(ECOpRef ecop){
  dout(10) << __func__ << " " << ecop->obc->obs.oi.soid << dendl;
  pMulOpPro->eval_writeread(ecop); 

  //writefull
  int iResult = process_new_op(ecop,2);
  if(0 != iResult){
    issue_repop(ecop);
  }else {
    //该分支不可到达
    assert(0);
    pMulOpPro->eval_write(DIRECT_EC_OP_LOCALSYNC,ecop);
    pMulOpPro->eval_write(DIRECT_EC_OP_COMMITTED,ecop);
    pMulOpPro->eval_write(DIRECT_EC_OP_APPLIED,ecop);
    pMulOpPro->delete_ecop_from_map(ecop);
    return;
  }
}

//ECWrite操作相关的类以及函数。
//函数功能: ECWrite操作的回调实体函数,将ECWrite列表中的已经应用的op反馈给客户端,并将没有应用的op应用到outdat中
//	    并记录已经应用的op的位置.
//参数:   [in]int type(1：applid；2：commit；3，local_sync)
//	[in,out]ECOpRef ecop
//返回值: void
void DirectECProcess::sub_write_reply(int type,ECOpRef ecop){
  dout(10) << __func__ << "(" << type << ", " << ecop->obc->obs.oi.soid 
       << " " << ecop->lasttid << ")" << dendl;

  //1)应用过的op，调用pg层传送过来的回调函数返回给客户端，未应用过的op,应用到内存中，并标记是否应用的位置。
  int iResult = pMulOpPro->eval_write(type,ecop);

  //2)all_applied_commit &&  all_applied 继续处理
  if(ecop->repop->all_applied && ecop->repop->all_committed){
    if(DIRECT_EC_NO_OP == iResult){//不需要提交事务,判断两个list是否为空,都为空则从map中删除
      pMulOpPro->delete_ecop_from_map(ecop);
      return;
    }

    ecop->repop->put();
    ecop->repop = NULL;
    iResult = process_new_op(ecop,iResult);
    if(0 != iResult){//需要提交事务
      issue_repop(ecop);
    }else {
      pMulOpPro->eval_write(DIRECT_EC_OP_LOCALSYNC,ecop);
      pMulOpPro->eval_write(DIRECT_EC_OP_COMMITTED,ecop);
      pMulOpPro->eval_write(DIRECT_EC_OP_APPLIED,ecop);
      pMulOpPro->delete_ecop_from_map(ecop);
      return;
    }
  }  
}

//函数功能: ECWrite_Read列表中对齐Append、delete setxattr等操作的回调实体函数,将ECWrite_Read列表中的已经应用的op反馈给客户端,并判断没有应用的op,是否是对齐的Append
//	   delete,setxattr等操作,是,则直接发送该操作,不是,则发送ECWrite_Read操作.
//参数:   [in]int type(1：applid；2：commit；3，local_sync)
//	[in,out]ECOpRef ecop
//返回值: void
void DirectECProcess::sub_append_reply(int type,ECOpRef ecop){
  dout(10) << __func__ << "(" << type << ", " << ecop->obc->obs.oi.soid 
       << " " << ecop->lasttid << ")" << dendl;
  
  //1)应用过的op，调用pg层传送过来的回调函数返回给客户端，未应用过的op,应用到内存中，并标记是否应用的位置。
  int iResult = pMulOpPro->eval_append(type,ecop);

  if(ecop->repop->all_applied && ecop->repop->all_committed ){
    if(DIRECT_EC_NO_OP == iResult){//不需要提交事务,判断两个list是否为空,都为空则从map中删除
      pMulOpPro->delete_ecop_from_map(ecop);
      return;
    }

    ecop->repop->put();
    ecop->repop = NULL;

    if (DIRECT_EC_WRITEREAD_OP == iResult){//发送ECWrite_Read操作,并退出该流程
      start_async_ecwrite_reads(ecop->old_obs,ecop); 
      return;
    }
    
    iResult = process_new_op(ecop,iResult);
    if(0 != iResult){
      issue_repop(ecop);
    } else {
      pMulOpPro->eval_write(DIRECT_EC_OP_LOCALSYNC,ecop);
      pMulOpPro->eval_write(DIRECT_EC_OP_COMMITTED,ecop);
      pMulOpPro->eval_write(DIRECT_EC_OP_APPLIED,ecop);
      pMulOpPro->delete_ecop_from_map(ecop);
      return;
    }
  } 
}

//函数功能: 清空过程数据(如:清空pMulOpPro::map_oid_ECOp,释放map中的资源)
//参数:  void
//返回值: void
void DirectECProcess::on_change(){
  pMulOpPro->on_change();
}

//函数功能: 将ecop中的outdata、map_xattr内容应用到Repop中，并生成Transaction,log.
//参数:  ECOpRef ecop
//参数: int type
//       0,不操作
//       1, 发送Write_Read操作；
//       2，发送Write操作；
//       3，发送Append操作；
//       4，发送SetAttr操作；
//       5，发送delete操作；
//返回值: int
// 0,不操作
//1,需要提交事务
int DirectECProcess::process_new_op(ECOpRef ecop,int type){
  //将ecop中的outdata、map_xattr内容应用到Repop中，并生成Transaction,log.
  int iResult = 1;

  assert(ecop->repop == NULL);
  ecop->repop = simple_repop_create(ecop->obc);
  ReplicatedPG::OpContext *ctx = ecop->repop->ctx;
  ObjectState& obs = ecop->old_obs;
  object_info_t& oi = obs.oi;
  const hobject_t& soid = oi.soid;
  PGBackend::PGTransaction* t = ctx->op_t;

  dout(10) << __func__ << " type " << type << dendl;
  ecop->dump(); // dump ecop info to log


  // set version & user_version for current merged op and all applied ops
  ObjectState& new_obs = ecop->next_op_obs;
  ctx->at_version = ecpg->get_next_version();
  ecop->repop->v = ctx->at_version;
  ctx->user_at_version = MAX(ecpg->info.last_user_version, new_obs.oi.user_version) + 1;
  if (ctx->at_version.version > ctx->user_at_version)
    ctx->user_at_version = ctx->at_version.version;

  object_stat_collection_t delta;
  pMulOpPro->set_version(ecop, delta);
  
  if( ecop->brmop || ecop->brmobj ) {
    ecop->obc->attr_cache.clear();
  }

  if (ecop->brmobj == new_obs.exists) {
    derr << "ecop->brmobj == new_obs.exists" << dendl;
    ecop->dump(0);
  }

  if (!obs.exists && ecop->brmobj) {
    delete t;
    ctx->op_t = NULL;
    return 0;
  }

  map<string, boost::optional<bufferlist> > &overlay = ecop->map_xattr;  
  map<string, boost::optional<bufferlist> > old_xattr;
  map<string, boost::optional<bufferlist> > new_xattr;
  map<string,bufferlist>::iterator iattrpos;
  map<string,bufferlist> &attr_cache = ecop->obc->attr_cache;
  map<string, boost::optional<bufferlist> >::iterator boostxattrpos;
  if(obs.exists && !ecop->brmobj) { 
    //2015-09-07 flc 添加限制条件 && !ecop->is_appendop_in_write_list//////////////////////////////////////
    if(ecop->outdata.first == 0 && !ecop->is_appendop_in_write_list) {
      ctx->mod_desc.rmobject(ctx->at_version.version);
      t->stash(soid,ctx->at_version.version);
      ctx->mod_desc.create();
      for(iattrpos = attr_cache.begin();iattrpos != attr_cache.end();++iattrpos) {
        if(!ECUtil::is_hinfo_key_string(iattrpos->first))
          new_xattr[iattrpos->first] = iattrpos->second;
      }
      for(boostxattrpos = overlay.begin();boostxattrpos != overlay.end();++boostxattrpos) {
        if(boostxattrpos->second) {
          new_xattr[boostxattrpos->first] = boostxattrpos->second;
          attr_cache[boostxattrpos->first] = boostxattrpos->second.get();
        }else{
          new_xattr.erase(boostxattrpos->first);
          attr_cache.erase(boostxattrpos->first);
        }
      }
    }else{
      //2015-09-09 flc 添加限制条件  ecop->old_obs.oi.size < ecop->outdata.first+ecop->outdata.second.length()//////////////////////////////////////
      if(ecop->outdata.first != (-1ULL) && ecop->outdata.second.length()>0 ){
        if( ecop->old_obs.oi.size < ecop->outdata.first+ecop->outdata.second.length()){
          dout(10) << __func__ << "mod_desc.append: buffer length=" << ecop->outdata.first+ecop->outdata.second.length() << " ,object length="<<ecop->old_obs.oi.size<<dendl;
          ctx->mod_desc.append(ecop->old_obs.oi.size);
        }
      }
      
      for(map<string,boost::optional<bufferlist> >::iterator ipos = overlay.begin();ipos != overlay.end();++ipos) {
        iattrpos = attr_cache.find(ipos->first);
        if(ipos->second) {
          if(iattrpos == attr_cache.end()) {
            old_xattr[ipos->first];
            attr_cache[ipos->first] = ipos->second.get();
          }else {
            old_xattr[ipos->first] = iattrpos->second;
            attr_cache[ipos->first] = ipos->second.get();
          }
        }else {
          if(iattrpos == attr_cache.end()) {
            old_xattr[ipos->first];
          }else {
            old_xattr[ipos->first] = iattrpos->second;
            attr_cache.erase(iattrpos);
          }          
        }
        new_xattr[ipos->first] = ipos->second;
      }
    }
  }
  if(!obs.exists) {
    ctx->mod_desc.create();
    for(boostxattrpos = overlay.begin();boostxattrpos != overlay.end();++boostxattrpos) {
      if(boostxattrpos->second) {
        new_xattr[boostxattrpos->first] = boostxattrpos->second;
        attr_cache[boostxattrpos->first] = boostxattrpos->second.get();
      }
    }
  }
  if(ecop->brmobj) {
    ctx->mod_desc.rmobject(ctx->at_version.version);
    t->stash(soid,ctx->at_version.version); 
  }else if(ecop->outdata.first == (-1ULL) || ecop->outdata.second.length() == 0){
    t->touch(soid);
  }else if(ecop->outdata.second.length() > 0){//
    if(!ecop->is_appendop_in_write_list){
      t->append(soid,ecop->outdata.first,ecop->outdata.second.length(),ecop->outdata.second);
    }else{//append//2015-09-07日 flc 添加 对齐的Append的操作、属性修改操作等,不重写已经存在的数据,///////////////////////////////// 
      dout(10) << __func__ << " buffer length=" << ecop->outdata.second.length() << " change to Append,object length="<<ecop->old_obs.oi.size<<dendl;
      ecop->is_appendop_in_write_list = false;
      if(ecop->outdata.second.length() > ecop->old_obs.oi.size){//对齐的Append的操作,不重写已经存在的数据,
        bufferlist bufappend;
        bufappend.substr_of(ecop->outdata.second,ecop->old_obs.oi.size,ecop->outdata.second.length()-ecop->old_obs.oi.size);
        t->append(soid,ecop->old_obs.oi.size,ecop->outdata.second.length()-ecop->old_obs.oi.size,bufappend);
      }else if(ecop->outdata.second.length() == ecop->old_obs.oi.size){//2015-09-08日//不影响数据的操作,如:属性修改等.
        t->touch(soid);
      }
    }
  }
  
  // wumq added to resolve scrub error: snapset.head_exists=false, but head exists
  ctx->new_snapset.head_exists = new_obs.exists;
  
  if (new_obs.exists) {
    object_info_t& noi = new_obs.oi;
    noi.version = ctx->at_version;
    noi.prior_version = obs.oi.version;
    noi.user_version = ctx->user_at_version;
    noi.last_reqid = ctx->reqid; //? not sure
    noi.mtime = ctx->mtime; // mtime comes from client requests
    noi.local_mtime = ceph_clock_now(ecpg->cct); // local_mtime comes from OSD

    bufferlist bv(sizeof(noi));
    ::encode(noi, bv);
    old_xattr[OI_ATTR] = attr_cache[OI_ATTR];
    new_xattr[OI_ATTR] = bv;
    attr_cache[OI_ATTR] = bv;

    // set SS_ATTR
    assert(soid.snap == CEPH_NOSNAP);
    bufferlist bss;
    ::encode(ctx->new_snapset, bss);
    old_xattr[SS_ATTR] = attr_cache[SS_ATTR];
    new_xattr[SS_ATTR] = bss;
    attr_cache[SS_ATTR] = bss;
  } else {
    new_obs.oi = object_info_t(soid);
  }

  if(old_xattr.size() > 0)
    ctx->mod_desc.setattrs(old_xattr);
  for(boostxattrpos = new_xattr.begin();boostxattrpos != new_xattr.end();++boostxattrpos) {
    if(boostxattrpos->second) {
      t->setattr(soid,boostxattrpos->first,boostxattrpos->second.get());
    }
    else
      t->rmattr(soid,boostxattrpos->first);
  }

  if (ecop->outdata.first != -1ull) {
    if (ecop->outdata.first + ecop->outdata.second.length() != new_obs.oi.size) {
      ecop->dump(0);

      JSONFormatter fmt(true);
      fmt.open_object_section("dump");
      fmt.dump_stream("oid") << soid;
      fmt.dump_unsigned("rep_tid", ecop->repop->rep_tid);
      static_cast<ECTransaction*>(t)->dump(fmt);
      ctx->mod_desc.dump(&fmt);
      fmt.close_section();

      std::ostringstream oss;
      fmt.flush(oss);
      dout(0) << oss.str() << dendl;
      assert(false);
    }
  }

  //修改log
  ctx->log.push_back(pg_log_entry_t(ecop->brmobj ? pg_log_entry_t::DELETE : pg_log_entry_t::MODIFY, 
        soid, ctx->at_version,
        ctx->obs->oi.version,
        ctx->user_at_version, ctx->reqid,
        ctx->mtime));
  ctx->log.back().mod_desc.claim(ctx->mod_desc);

  // update stats
  ctx->bytes_written = ctx->op_t->get_bytes_written();
  ecpg->info.stats.stats.add(delta);
  for (set<pg_shard_t>::iterator i = ecpg->backfill_targets.begin();
       i != ecpg->backfill_targets.end(); ++i) {
    pg_info_t& pinfo = ecpg->peer_info[*i];
    if (soid <= pinfo.last_backfill)
      pinfo.stats.stats.add(delta);
    else if (soid <= ecpg->last_backfill_started)
      ecpg->pending_backfill_updates[soid].stats.add(delta);
  }

  // check scrubber
  if (ecpg->scrubber.active) {
    if (soid < ecpg->scrubber.start)
      ecpg->scrub_cstat.add(delta);
    else if (soid < ecpg->scrubber.end) {
      dout(0) << soid << " in scrub range and state is "
           << PG::Scrubber::state_string(ecpg->scrubber.state) << dendl;
      ecpg->scrubber.last_write_update = ctx->at_version;
    }
  }

  // apply new object state
  obs = new_obs;
  ecop->obc->obs = new_obs; // wumq
  ecop->brmop = false;
  ecop->obc->ssc->exists = true;
  ecop->obc->ssc->snapset = ctx->new_snapset;
  return iResult;
}

void DirectECProcess::issue_repop(ECOpRef ecop) {  
  ReplicatedPG::OpContext *ctx = ecop->repop->ctx;
  const hobject_t& soid = ecop->old_obs.oi.soid;
  PGBackend::PGTransaction* t = ctx->op_t;

  ecpg->calc_trim_to(); //?

  const string& df = ecpg->cct->_conf->ectransaction_dump_file;
  if (df.length()) {
    JSONFormatter fmt(true);
    fmt.open_object_section("dump");
    fmt.dump_stream("oid") << soid;
    fmt.dump_unsigned("rep_tid", ecop->repop->rep_tid);
    static_cast<ECTransaction*>(t)->dump(fmt);
    fmt.close_section();

    ofstream of(df.c_str(), ios_base::out | ios_base::app);
    fmt.flush(of);
    of << std::endl;
    of.close();
  }
  
  Context *onapplied_sync = NULL;
  Context *on_all_applied = NULL;
  Context *on_all_commit = NULL;
  if(ecop->list_clientop_writeread.size()) {
    onapplied_sync = new AppendOndiskWriteUnlock(this,ecop);
    on_all_applied = new AppendAppliedContext(this,ecop);
    on_all_commit = new AppendCommittedContext(this,ecop);
  } else {
    assert(ecop->list_clientop_write.size());
    onapplied_sync = new WriteOndiskWriteUnlock(this,ecop);
    on_all_applied = new WriteAppliedContext(this,ecop);
    on_all_commit = new WriteCommittedContext(this,ecop);
  }
  
  pgbackend->submit_transaction(soid,ctx->at_version,t,
      ecpg->pg_trim_to,
      ecpg->min_last_complete_ondisk,
      ctx->log,
      ctx->updated_hset_history,
      onapplied_sync,
      on_all_applied,
      on_all_commit,
      ecop->repop->rep_tid,
      ctx->reqid,
      ctx->op);
  ctx->op_t = NULL;
}

ReplicatedPG::RepGather *DirectECProcess::simple_repop_create(ObjectContextRef obc)
{
  dout(20) << __func__ << " " << obc->obs.oi.soid << dendl;

  vector<OSDOp> ops;
  ceph_tid_t rep_tid = ecpg->osd->get_tid();
  osd_reqid_t reqid(ecpg->osd->get_cluster_msgr_name(), 0, rep_tid);
  ReplicatedPG::OpContext *ctx = new ReplicatedPG::OpContext(
       OpRequestRef(), reqid, ops, &obc->obs, obc->ssc, ecpg);
  ctx->op_t = pgbackend->get_transaction();
  ctx->obc = obc;

  ReplicatedPG::RepGather *repop = new ReplicatedPG::RepGather(
       ctx, obc, rep_tid, ecpg->info.last_complete);
  repop->start = ceph_clock_now(ecpg->cct);
  return repop;
}

