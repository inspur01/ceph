/******************************************************************************
 
                   版权所有 (C), 2015-2099, 浪潮电子信息产业股份有限公司
 
  ******************************************************************************
   文 件 名   : GrpFileQuota.h
   版 本 号   : 初稿
   作    者   : lvqiang
   生成日期   : 2016年5月16日
   最近修改   :
   功能描述   : 用户文件数配额
   函数列表   :
   修改历史   :
   1.日  期   : 2016年5月16日
     作  者   : lvqiang
   修改内容   : 创建文件
 
 ******************************************************************************/
#ifndef CEPH_COMMON_GRP_FILE_QUOTA_H
#define CEPH_COMMON_GRP_FILE_QUOTA_H

#include "include/types.h"
#include "include/filepath.h"
#include "include/elist.h"

#include "include/Context.h"

#include "include/buffer.h"

static const version_t GRP_FILE_QUOTA_VERSION = 100;


/*
 * GrpFileQuota
 */
class GrpFileQuota {
  public:
    version_t version;
    uint32_t grp_file_cnt; // 用户文件数
    int32_t vary_cnt;       // 变化量
    gid_t gid;
    int32_t vary_oldcnt;
  public:	
	GrpFileQuota() : version(GRP_FILE_QUOTA_VERSION), grp_file_cnt(0), vary_cnt(0),vary_oldcnt(0) {}
	GrpFileQuota(gid_t tgid) : version(GRP_FILE_QUOTA_VERSION), grp_file_cnt(0), vary_cnt(0), gid(tgid),vary_oldcnt(0) {}

	void SetUid(gid_t tgid) 
	{
	  gid = tgid;
	}

	gid_t GetUid() 
	{
	  return gid;
	}
	
    void encode(bufferlist &bl) const 
	{
	  ::encode(version, bl);
	  ::encode(grp_file_cnt, bl);
      ::encode(vary_cnt, bl);
	  ::encode(gid, bl);
	}

	void decode(bufferlist::iterator& bl) 
	{
	  ::decode(version, bl);
	  ::decode(grp_file_cnt, bl);
	  ::decode(vary_cnt, bl);
	  ::decode(gid, bl);
	}

    void update(int32_t vary_cnt);
    void dump(Formatter *f) const 
	{
      f->dump_unsigned("version", version);
      f->dump_unsigned("grp_file_cnt", grp_file_cnt);
      f->dump_unsigned("vary_cnt", vary_cnt);
      f->dump_unsigned("gid", gid);
    }

    static void generate_test_instances(list<GrpFileQuota*>& o) 
	{
      //o.push_back(new GrpFileQuota());
    }
};
WRITE_CLASS_ENCODER(GrpFileQuota);

#endif 

