/******************************************************************************
 
                   版权所有 (C), 2015-2099, 浪潮电子信息产业股份有限公司
 
  ******************************************************************************
   文 件 名   : UserQuotaData.h
   版 本 号   : 初稿
   作    者   : lvqiang
   生成日期   : 2016年3月22日
   最近修改   :
   功能描述   : 用户配额数据类
   函数列表   :
   修改历史   :
   1.日  期   : 2016年3月22日
     作  者   : lvqiang
   修改内容   : 创建文件
 
 ******************************************************************************/
#ifndef CEPH_COMMON_USER_QUOTA_DATA_H
#define CEPH_COMMON_USER_QUOTA_DATA_H

#include "include/types.h"
#include "include/filepath.h"
#include "include/elist.h"

#include "include/Context.h"

#include "include/buffer.h"

#define E_QUOTA_FORMAT        600 
#define E_QUOTA_EXIST         601 
#define E_QUOTA_NOTEXIST      602
#define E_QUOTA_HARD_SOFT     603
#define E_QUOTA_HARD_USED     604
#define E_QUOTA_HARD_SYSTEM   605
#define E_QUOTA_HAVE_FILE     606
#define E_QUOTA_PRECISION     607
#define E_QUOTA_MDS           608
#define E_QUOTA_USER_GROUP    609
#define E_QUOTA_OVERFLOW      610
#define E_QUOTA_INVALID_USER  611
#define E_QUOTA_INVALID_GRP   612
#define E_QUOTA_USER_EXCEED_GRP   613  // 设置的用户硬配额超过用户组剩余的硬配额
#define E_QUOTA_GRP_BELOW_USERS   614  // 用户组配额小于组内所有用户的硬阈值之和




#define MIN_PRECISION         (200<<20)   /*200M*/
#define ADVANCE_SIZE		  (100<<20)
//#define MAX_QUOTA_AMOUNT      2048
//#define MAX_GROUP_QUOTA_AMOUNT      256



static const char USER_QUOTA_SET = 'S';
static const char USER_QUOTA_DELETE = 'D';
static const char USER_QUOTA_UPDATE = 'U';
static const char USER_QUOTA_MOD    = 'M';
static const char USER_QUOTA_CLEAR  = 'C';
static const char USER_QUOTA_UPDATE_FILE = 'F';

static const char QUOTA_TYPE_USER = 'U';
static const char QUOTA_TYPE_GRP  = 'G';


static const version_t QUOTA_VERSION = 100;


/*
 * UserQuotaData
 */
class UserQuotaData {
  public:
    version_t version;
    uint64_t  hardlimit; /* In bsize units. */
    uint64_t  softlimit; /* In bsize units. */
    uint64_t  used_bytes;
    int64_t   buf_used;
    int64_t   buf_oldused;
    uint64_t  accuracy; /*200M ~ 1T*/ 
    uid_t     uid;
	gid_t     gid;
	char      op_type;
	bool	  warn_rate;
	
	UserQuotaData() : version(QUOTA_VERSION) 
	{
      hardlimit   = 0;
      softlimit   = 0;
      used_bytes  = 0;
      buf_used    = 0;
      buf_oldused = 0;
      accuracy    = 0;     
      op_type     = '\0';
	  warn_rate = true;
	}
	
	
    void encode(bufferlist &bl) const
	{
      ::encode(hardlimit, bl);
      ::encode(softlimit, bl);
      ::encode(used_bytes, bl);
      ::encode(buf_used, bl);
      ::encode(uid, bl);
	  ::encode(gid, bl);
      ::encode(accuracy, bl);
      ::encode(version, bl);
      ::encode(op_type, bl);
	}

	void decode(bufferlist::iterator& bl)
	{
      ::decode(hardlimit, bl);
      ::decode(softlimit, bl);
      ::decode(used_bytes, bl);
      ::decode(buf_used, bl);
      ::decode(uid, bl);
	  ::decode(gid, bl);
      ::decode(accuracy, bl);
      ::decode(version, bl);
      ::decode(op_type, bl);
	}

    void modify(UserQuotaData& user_quota);
    void dump(Formatter *f) const 
	{
      f->dump_unsigned("hardlimit", hardlimit);
      f->dump_unsigned("softlimit", softlimit);
      f->dump_unsigned("used_bytes", used_bytes);
      f->dump_unsigned("buf_used", buf_used);
      f->dump_unsigned("accuracy", accuracy);
	  f->dump_unsigned("uid", uid);
	  f->dump_unsigned("gid", gid);
	  f->dump_unsigned("version", version);
	  f->dump_unsigned("op_type", op_type);
    }

    static void generate_test_instances(list<UserQuotaData*>& o)
    {
        //o.push_back(new UserQuotaData());
    }
};
WRITE_CLASS_ENCODER(UserQuotaData);

#endif 

