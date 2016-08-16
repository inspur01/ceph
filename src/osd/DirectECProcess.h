// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Copyright (C) 2013 Cloudwatt <libre.licensing@cloudwatt.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_DIRECTECPROCESS_H
#define CEPH_DIRECTECPROCESS_H

#include <boost/optional/optional_io.hpp>
#include <boost/tuple/tuple.hpp>

#include "include/assert.h" 
#include "common/cmdparse.h"

#include "ErasureCodePG.h"
//#include "ReplicatedPG.h"

#include "include/memory.h"


class ErasureCodePG;

enum {
  DIRECT_EC_OP_APPLIED = 1,
  DIRECT_EC_OP_COMMITTED = 2, 
  DIRECT_EC_OP_LOCALSYNC = 3
};
enum {
  MERGE_OP_WRITEREAD = 0,
  MERGE_OP_APPEND = 1, 
  MERGE_OP_WRITE = 2
};

enum {
  DIRECT_EC_NO_OP = 0,        //无后续操作
  DIRECT_EC_WAITING = 0,      //等待
  DIRECT_EC_WRITEREAD_OP = 1, //发送Write_Read操作
  DIRECT_EC_WRITEFULL_OP = 2, //发送WriteFull操作
  DIRECT_EC_APPEND_OP = 3,    //发送Append操作
  DIRECT_EC_DELETE_OP = 4     //发送DELETE操作
};



// 存储了客户端请求信息
struct DirectECOp{
  Context * on_local_applied_sync;//写入本地磁盘成功的回调函数
  Context * on_all_applied;//EC写入applied的回调
  Context * on_all_commit; //EC写入commit的回调
  ReplicatedPG::RepGather* repop;//
  ceph_tid_t rep_tid;
  DirectECOp(Context *onsync,Context *onallapplied,Context *onallcommit,ReplicatedPG::RepGather* prepop)
    :on_local_applied_sync(onsync),on_all_applied(onallapplied),
    on_all_commit(onallcommit),repop(prepop),rep_tid(prepop->rep_tid){}
  ~DirectECOp(){
    delete on_local_applied_sync;
    delete on_all_applied;
    delete on_all_commit;  

    on_local_applied_sync = NULL;
    on_all_applied = NULL;
    on_all_commit = NULL;
    repop = NULL;//什么地方delete该指针//TODO////////
  }

  void dump(int level); // 供ECOp::dump()调用
};

// 直接纠删OP
class ECOp{
  public:
    ReplicatedPG::RepGather * repop;
    pair<uint64_t,bufferlist> outdata;//数据
    map<string,boost::optional<bufferlist> > map_xattr;//存储了最新的对象属性
    bool brmobj;        //对象是否被删除
    bool brmop;        //合并的操作中是否有delelte操作
    ceph_tid_t lasttid; //记录列表中op应用到的位置
    //1.在check_osd_ops函数中,修改new_obs;
    //2.do_osd_ops中将new_obs更新到内存obc中;
    //3.提交事务之前需要将合并操作的最后一个操作repop->ctx->new_obs 赋值给old_obs
    ObjectContextRef obc;
    ObjectState old_obs;//存储合并操作前对象的信息
    ObjectState next_op_obs;//记录合并操作的最后一个操作的新状态指针
    std::list<DirectECOp *> list_clientop_writeread;//当前读取中的任务列表
    std::list<DirectECOp *> list_clientop_write; //当前写入队列中的任务列表
    bool is_appendop_in_write_list;//在list_clientop_write列表中,后续的操作是对齐的Append
    ECOp(){
      outdata.first = -1ull;

      brmobj = false;
      brmop = false;
      repop = NULL;
      is_appendop_in_write_list = false;
    }
    ~ECOp(){
      for(list<DirectECOp *>::iterator iter = list_clientop_writeread.begin();
          iter != list_clientop_writeread.end();
          ++iter){
        delete *iter;

        *iter = NULL;
      }
      list_clientop_writeread.clear();	

      for(list<DirectECOp *>::iterator iter = list_clientop_write.begin();
          iter != list_clientop_write.end();
          ++iter){
        delete *iter;

        *iter = NULL;
      }
      list_clientop_write.clear();

      if(repop != NULL){
        repop->put();
        repop = NULL;
      }

    }

    /*
     * 用于调试的方法: 输出ecop的信息到OSD日志
     * @param level: 日志级别
     */
    void dump(int level = 20);

};
typedef ceph::shared_ptr<ECOp> ECOpRef; 


//多op处理类
class MulOpProcess{
  public:
    MulOpProcess(uint64_t _stripe_width, ErasureCodePG* _pg):stripe_width(_stripe_width), ecpg(_pg) {}
    ~MulOpProcess(){
      map_oid_ECOp.clear();
    }
  private:
    uint64_t stripe_width;//条带长度
    map<hobject_t,ECOpRef> map_oid_ECOp;//存储了当前oid的所有操作
    ErasureCodePG* ecpg; // used for dout log

  public:
    //函数功能:将当前操作添加到map_oid_ECOp中
    //参数: [in]RepGather* repop
    //      [in]Context *on_locall_applied_sync
    //      [in]Context *on_all_applied
    //      [in]Context *on_all_commit
    //      [in,out]ECOpRef& ecop
    //返回值:0, 等待处理
    //       1, 发送Write_Read操作；
    //       2，发送Write操作；
    //       3，发送Append操作；
    //       4，发送delete操作；
    int add_op_to_map(ReplicatedPG::RepGather* repop,
        Context *on_locall_applied_sync,
        Context *on_all_applied,
        Context *on_all_commit,
        ECOpRef& ecop);

    //函数功能:ECWrite_Read操作读取数据完成后；将list_clientop_writeread列表中的操作应用到读取的数据中,并将操作从列表
    //         list_clientop_writeread中转移到list_clientop_write列表中,并标志该操作已经应用.
    //参数:[in,out]boost::shared_ptr<ECOp> ecop
    //返回值:void
    void eval_writeread(ECOpRef ecop);//参数用智能指针

    //函数功能:处理返回的write操作；会把所有已经处理的write操作，执行applid、commit操作；
    //         如果状态是applid且commit的，那么把该任务从任务队列中删除；
    //参数:   [in]int type (1，applid；2，commit，3，local_sync)
    //	  [in,out]ECOpRef ecop
    //返回值: 0，不处理；
    //	    !0，继续Write；
    int eval_write(int type,ECOpRef ecop);//参数用智能指针

    //函数功能:处理返回的Append操作；会把所有已经处理的Append操作，执行applid、commit操作；
    //         如果状态是applid且commit的，那么把该任务从任务队列中删除；
    //参数:   [in]int type (1，applid；2，commit，3，local_sync)
    //	  [in,out]ECOpRef ecop
    //返回值: 0，不处理；
    //        1, 发送Write_Read操作；
    //        2，发送Write操作；
    //        3，发送Append操作；
    int eval_append(int type,ECOpRef ecop); 

    //函数功能:从map_oid_ECOp中获取key值为oid所对应的Value值 ECOp智能指针
    //参数:   [in]hobject_t oid
    //返回值: NULL,  map_oid_ECOp中没有oid所对应的指针
    //        ECOpRef ecop，oid所对应的ECOp智能指针
    ECOpRef  get_ecop(hobject_t oid);
    /*
     * 函数功能:遍历列表中已经应用的op, 设置其version和user_version
     * 参数：[in,out] ECOpRef ecop, 待提交请求所属的ECOp
     *     [out] delta，用于更新对象存储的统计信息
     */
    void set_version(ECOpRef ecop, object_stat_collection_t& delta);
    //函数功能: 清空map_oid_ECOp,释放map中的资源.
    //参数:  void
    //返回值: void
    void on_change();
    //函数功能: 从map_oid_ECOp中删除特定的ecop.
    //参数: [in]ECOpRef ecop
    //返回值: void
    void delete_ecop_from_map(ECOpRef ecop);

    /*
     *ecop如果没有生成之前，就是在add_op_to_map中调用的时候，直接使用空的ecop就可以
     *
     *Return: 1,WriteFull;2,DELETE;3,Append;4,WriteRead;     
     */
    int judge_op_type(ReplicatedPG::RepGather* repop,ECOpRef& ecop);

private:
   
//2015-09-06日 flc 添加 //用于判断write列表中的操作是否是对齐Append操作函数//begin//////////////////////    
    //函数功能: 判断操作是否是对齐的Append操作
    //参数: [in]ReplicatedPG::RepGather* repop
    //      [in]ECOpRef& ecop
    //返回值: bool
    //        ture:write列表中的操作是对齐的Append操作;
    //        false:非对齐的Append操作;
    bool is_appendop_in_writelist(ReplicatedPG::RepGather* repop,ECOpRef& ecop);
//2015-09-06日 flc 添加 //用于判断write列表中的操作是否是对齐Append操作函数//end////////////////////////    

 
    //函数功能: 合并操作,根据type的类型进行不同列表的处理,1 == type时,ECWrite_Read操作读取数据完成后调用本函数,合并
    //         操作,并将ECWrite_Read列表中的操作转移到ECWrite列表中;2 == type时,对齐的Append或者delete或者setxattr操作
    //         完成后,处理后续的操作如果还是对齐的Append,delete,setxattr其中之一,直接发送对齐的Append,delete,setxattr操作
    //         如果不是其中之一,则发送Write_Read操作;3==type时,如果有后续操作合并操作发送Write操作,如果没有后续操作则返回0
    //参数:   [in]int type (1，ECWrite_Read；2，对齐的Append,delete,setxattr，3，ECWrite)
    //	  [in,out]ECOpRef ecop
    //返回值: 0,无后续操作
    //        1,发送Write_Read操作；
    //        2，发送Write操作；
    //        3，发送Append操作；
    //        4，发送SetAttr操作；
    //        5，发送delete操作；
    int merge_directecop(int type,ECOpRef ecop);

    //函数功能: 将DirectECOp应用到outdata上,并记录属性.
    //参数:   [in,out]pair<uint64_t,bufferlist> &outdata
    //	  [in,out]map<std::string,boost::optional<bufferlist>> &xattr
    //        [int]DirectECOp * directecop;
    //        [in,out]ECOpRef ecop 
    //返回值: 0,成功
    //        -1,异常
    int apply_ops_to_cache(pair<uint64_t,bufferlist> & outdata,map<string,boost::optional<bufferlist> > &xattr,
        DirectECOp * directecop,ECOpRef ecop);

    //函数功能: 合并CEPH_OSD_OP_WRITE,ZERO操作到内存中.
    //参数:   [in]OSDOp &osd_op
    //	[in,out]pair<uint64_t,bufferlist> &outdata
    //        [in]bool bwritezero
    //返回值: void
    void apply_write_to_cache(OSDOp &osd_op,pair<uint64_t,bufferlist> &outdata,bool bwritezero=false);

    //函数功能: 合并CEPH_OSD_OP_TRUNCATE操作到内存中.
    //参数:   [in]OSDOp &osd_op
    //	  [in,out]pair<uint64_t,bufferlist> &outdata
    //返回值: void
    void apply_truncate_to_cache(OSDOp &osd_op,pair<uint64_t,bufferlist> &outdata,int ioldlength);


};


//直接纠删的处理类
class DirectECProcess{
  public:    
    uint64_t stripe_width;//条带长度
    MulOpProcess *pMulOpPro;//多op处理类指针
    PGBackend *   pgbackend;//ECBackend指针
    ErasureCodePG * ecpg;//pg的指针
  public:
    DirectECProcess(uint64_t _stripe_width,PGBackend * _pgbackend,ErasureCodePG *_ecpg)
      :stripe_width(_stripe_width),pgbackend(_pgbackend),ecpg(_ecpg){
        pMulOpPro = new MulOpProcess(_stripe_width, ecpg);
      }
    virtual ~DirectECProcess(){
      if(NULL != pMulOpPro){
        delete pMulOpPro;
        pMulOpPro = NULL;
      }
    }  
  public:
    //函数功能:ECPG在may_write||may_chache为真时，调用check_osd_ops。check_osd_ops主要有两个功能：
    //        (1) 判断那些操作是不是应该交给DirectECProcess处理。(2)判断操作的参数有没有错误。
    //参数:   [in]ReplicatedPG::OpContext * ctx
    //返回值: int
    //	  < 0：某个操作的参数有错误，上层应当结束处理并将该错误回复给客户端。
    //	  = 0：DirectECProcess无法处理该请求，上层应当按原有流程处理。
    //	  > 0：DirectECProcess需要处理该请求，上层应当调用DirectECProcess::do_osd_ops，进入直接纠删的处理流程。
    int check_osd_ops(ReplicatedPG::OpContext * ctx);
    //函数功能:ECPG在check_osd_ops()返回值大于0时，将调用该方法进入直接纠删的处理流程。
    //         在调用之前，要构造repop对象，以及三个回调。
    //参数: [in]RepGather* repop
    //      [in]Context *on_locall_applied_sync
    //      [in]Context *on_all_applied
    //      [in]Context *on_all_commit
    //返回值:void
    void do_osd_ops(ReplicatedPG::RepGather* repop,
        Context *on_locall_applied_sync,
        Context *on_all_applied,
        Context *on_all_commit);

    //函数功能: ECWrite_Read操作的回调实体函数,将ECWrite_Read列表中的op应用到outdat中,并将op从ECWrite_Read列表转移到ECWrite
    //	     列表中,记录已经应用的op.
    //参数:   [in]ReplicatedPG::OpContext *pctx
    //	   [in,out]ECOpRef ecop
    //返回值: void
    void sub_write_read_reply(ECOpRef ecop);
    //函数功能: ECWrite操作的回调实体函数,将ECWrite列表中的已经应用的op反馈给客户端,并将没有应用的op应用到outdat中
    //	    并记录已经应用的op的位置.
    //参数:   [in]int type(1：applid；2：commit；3，local_sync)
    //	   [in,out]ECOpRef ecop
    //返回值: void
    void sub_write_reply(int type,ECOpRef ecop);
    //函数功能: ECWrite_Read列表中对齐Append、delete setxattr等操作的回调实体函数,将ECWrite_Read列表中的已经应用的op反馈给客户端,并判断没有应用的op,是否是对齐的Append
    //	   delete,setxattr等操作,是,则直接发送该操作,不是,则发送ECWrite_Read操作.
    //参数:   [in]int type(1：applid；2：commit；3，local_sync)
    //	  [in,out]ECOpRef ecop
    //返回值: void
    void sub_append_reply(int type,ECOpRef ecop);
    //函数功能: 清空过程数据(如:清空pMulOpPro::map_oid_ECOp,释放map中的资源)
    //参数:  void
    //返回值: void
    void on_change();

    //下面这两个函数实现不太清晰,//TODO /////////////////////
    int process_new_op(ECOpRef ecop,int type);//int type,操作类型,1:注册write回调;2:注册append回调
    void issue_repop(ECOpRef ecop);

  private:
    //函数功能: 发送ECWrite_Read操作,根据opcontext获取对象的信息(对象的大小),读取的数据存储在ecOp->outdata中.
    //参数:  [in]ObjectState obs
    //	    [in,out]ECOpRef ecop
    //返回值: void
    void start_async_ecwrite_reads(ObjectState& obs,ECOpRef ecOp);
    ReplicatedPG::RepGather * simple_repop_create(ObjectContextRef obc);
};

//ECWrite_Read的回调类型//
struct WriteReadContext : public Context {
  DirectECProcess *dECProcess;
  ECOpRef ecop;
  WriteReadContext(
      DirectECProcess *decp,
      ECOpRef _ecop) : dECProcess(decp), ecop(_ecop) {}
  void finish(int r) {   
    dECProcess->sub_write_read_reply(ecop);
  }
  ~WriteReadContext() {}
};
struct WriteReadFillInExtent : public Context {  
  WriteReadFillInExtent() {}
  void finish(int _r) {}
};


//ECWrite操作的committed回调类型
class WriteCommittedContext : public Context {
  DirectECProcess *dECProcess;
  ECOpRef ecop;
  public:
  WriteCommittedContext(DirectECProcess *ecpgprocess, ECOpRef _ecop)
    : dECProcess(ecpgprocess), ecop(_ecop) {}
  void finish(int) {
    dECProcess->sub_write_reply(DIRECT_EC_OP_COMMITTED,ecop);
  }
};
//ECWrite操作的Applied回调类型
class WriteAppliedContext : public Context {
  DirectECProcess *dECProcess;
  ECOpRef ecop;
  public:
  WriteAppliedContext(DirectECProcess *ecpgprocess, ECOpRef _ecop)
    : dECProcess(ecpgprocess), ecop(_ecop) {}
  void finish(int) {
    dECProcess->sub_write_reply(DIRECT_EC_OP_APPLIED,ecop);
  }
};
//ECWrite操作的OndiskWriteUnlock回调类型
struct WriteOndiskWriteUnlock : public Context
{
  DirectECProcess *dECProcess;
  ECOpRef ecop;
  WriteOndiskWriteUnlock(DirectECProcess * _ecpgprocess, ECOpRef _ecop) 
    : dECProcess(_ecpgprocess),ecop(_ecop) {}
  void finish(int r)
  {
    dECProcess->sub_write_reply(DIRECT_EC_OP_LOCALSYNC,ecop);
  }
};


//ECAppend操作的Committed回调类型
class AppendCommittedContext : public Context {
  DirectECProcess *dECProcess;
  ECOpRef ecop;
  public:
  AppendCommittedContext(DirectECProcess *ecpgprocess, ECOpRef _ecop)
    : dECProcess(ecpgprocess), ecop(_ecop) {}
  void finish(int) {
    dECProcess->sub_append_reply(DIRECT_EC_OP_COMMITTED,ecop);
  }
};
//ECAppend操作的Applied回调类型
class AppendAppliedContext : public Context {
  DirectECProcess *dECProcess;
  ECOpRef ecop;
  public:
  AppendAppliedContext(DirectECProcess *ecpgprocess, ECOpRef _ecop)
    : dECProcess(ecpgprocess), ecop(_ecop) {}
  void finish(int) {
    dECProcess->sub_append_reply(DIRECT_EC_OP_APPLIED,ecop);
  }
};
////ECAppend操作的OndiskWriteUnlock回调类型
struct AppendOndiskWriteUnlock : public Context
{
  DirectECProcess *dECProcess;
  ECOpRef ecop;
  AppendOndiskWriteUnlock(DirectECProcess *ecpgprocess, ECOpRef _ecop) 
    : dECProcess(ecpgprocess),ecop(_ecop) {}
  void finish(int r)
  {
    dECProcess->sub_append_reply(DIRECT_EC_OP_LOCALSYNC,ecop);
  }
};

#endif
