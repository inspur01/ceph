/******************************************************************************
 
                   版权所有 (C), 2015-2099, 浪潮电子信息产业股份有限公司
 
  ******************************************************************************
   文 件 名   : GrpQuotaData.h
   版 本 号   : 初稿
   作    者   : lvqiang
   生成日期   : 2016年5月16日
   最近修改   :
   功能描述   : 用户组配额数据类
   函数列表   :
   修改历史   :
   1.日  期   : 2016年5月16日
     作  者   : lvqiang
   修改内容   : 创建文件
 
 ******************************************************************************/
#ifndef CEPH_COMMON_GRP_QUOTA_DATA_H
#define CEPH_COMMON_GRP_QUOTA_DATA_H

#include "include/types.h"
#include "include/filepath.h"
#include "include/elist.h"

#include "include/Context.h"

#include "include/buffer.h"


static const char GRP_QUOTA_SET = 'S';
static const char GRP_QUOTA_DELETE = 'D';
static const char GRP_QUOTA_UPDATE = 'U';
static const char GRP_QUOTA_MOD    = 'M';
static const char GRP_QUOTA_CLEAR  = 'C';
static const char GRP_QUOTA_UPDATE_FILE = 'F';

static const version_t GRP_QUOTA_VERSION = 100;


/*
 * GrpQuotaData
 */
class GrpQuotaData {
  public:
    version_t version;
    uint64_t  hardlimit; /* In bsize units. */
    uint64_t  softlimit; /* In bsize units. */
    uint64_t  used_bytes;
    int64_t   buf_used;
    int64_t   buf_oldused;
    uint64_t  accuracy; /*200M ~ 1T*/ 
	gid_t     gid;
	char      op_type;
	bool	  warn_rate;
	uint64_t  free_quota; /*剩余可分配空间*/
	
	GrpQuotaData() : version(GRP_QUOTA_VERSION) 
	{
      hardlimit   = 0;
      softlimit   = 0;
      used_bytes  = 0;
      buf_used    = 0;
      buf_oldused = 0;
      accuracy    = 0;   
      op_type     = '\0';
	  warn_rate = true;
	  free_quota = 0;
	}	
	
    void encode(bufferlist &bl) const 
	{
      ::encode(hardlimit, bl);
      ::encode(softlimit, bl);
      ::encode(used_bytes, bl);
      ::encode(buf_used, bl);
	  ::encode(gid, bl);
      ::encode(accuracy, bl);
      ::encode(version, bl);
      ::encode(op_type, bl);
	  ::encode(free_quota, bl);
	}

	void decode(bufferlist::iterator& bl) 
	{
      ::decode(hardlimit, bl);
      ::decode(softlimit, bl);
      ::decode(used_bytes, bl);
      ::decode(buf_used, bl);
     // ::decode(uid, bl);
	  ::decode(gid, bl);
      ::decode(accuracy, bl);
      ::decode(version, bl);
      ::decode(op_type, bl);
	  ::decode(free_quota, bl);
	}

    void modify(GrpQuotaData& user_quota);
    void dump(Formatter *f) const 
	{
      f->dump_unsigned("hardlimit", hardlimit);
      f->dump_unsigned("softlimit", softlimit);
      f->dump_unsigned("used_bytes", used_bytes);
      f->dump_unsigned("buf_used", buf_used);
      f->dump_unsigned("accuracy", accuracy);
	  //f->dump_unsigned("uid", uid);
	  f->dump_unsigned("gid", gid);
	  f->dump_unsigned("version", version);
	  f->dump_unsigned("op_type", op_type);
	  f->dump_unsigned("free_quota", free_quota);
    }

    static void generate_test_instances(list<GrpQuotaData*>& o) 
	{
      //o.push_back(new GrpQuotaData());
    }
};
WRITE_CLASS_ENCODER(GrpQuotaData);

#endif 

