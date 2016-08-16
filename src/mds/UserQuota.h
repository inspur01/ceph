/******************************************************************************
 
                   版权所有 (C), 2015-2099, 浪潮电子信息产业股份有限公司
 
  ******************************************************************************
   文 件 名   : UserQuota.h
   版 本 号   : 初稿
   作    者   : lvqiang
   生成日期   : 2016年3月23日
   最近修改   :
   功能描述   : 用户配额数据类
   函数列表   :
   修改历史   :
   1.日  期   : 2016年3月23日
     作  者   : lvqiang
   修改内容   : 创建文件
 
 ******************************************************************************/
#ifndef CEPH_COMMON_USER_QUOTA_H
#define CEPH_COMMON_USER_QUOTA_H

#include "include/types.h"
#include "include/filepath.h"
#include "include/elist.h"

#include "include/Context.h"

#include "include/buffer.h"
#include "common/UserQuotaData.h"
#include "common/UserFileQuota.h"
#include "common/GrpQuotaData.h"
#include "common/GrpFileQuota.h"
#include "common/DirQuotaData.h"

class MDS;
class MDCache;

class QuotaMgrBase {
public:
  MDCache *mdcache;
  MDS *mds;  
  uint64_t version;
  inodeno_t ino;
  bool is_change;
  bool is_used;    // 判断配额功能是否是从未使用过

public:
  QuotaMgrBase(MDCache *c, MDS *m, uint64_t v=0) : mdcache(c), mds(m), version(v), ino(MDS_INO_USER_QUOTA), is_change(false), is_used(false) {}
  virtual ~QuotaMgrBase() {}
  
  virtual void store(MDSInternalContextBase *fin) = 0;
  virtual void fetch(MDSInternalContextBase *fin) = 0;
  virtual void _fetched(bufferlist& bl, Context *fin) = 0;

	uint64_t get_version()
	{
		return version;
	}

	uint64_t get_inc_version()
	{
		return version++;
	}

	void set_version(uint64_t v)
	{
		version = v;
	}

	void inc_version()
	{
		++version;
	}

	void set_change()
	{
		is_change = true;
	}

	void set_change_ok()
	{
		is_change = false;
	}

	bool is_nochange()
	{
		return !is_change;
	}

	void set_used()
	{
		is_used = true;
	}

	bool is_noused()
	{
		return !is_used;
	}
};

class UserQuotaMgr : public QuotaMgrBase {
public:
  map<uid_t, UserQuotaData> user_quota_map;      // 用户配额map
  
public:
  UserQuotaMgr(MDCache *c, MDS *m, uint64_t v=0) : QuotaMgrBase(c, m, v) {}
  void store(MDSInternalContextBase *fin);
  void fetch(MDSInternalContextBase *fin);
  void _fetched(bufferlist& bl, Context *fin);

  UserQuotaData* get_user_quota(uid_t uid)
  {
    if (user_quota_map.count(uid))
      return &(user_quota_map[uid]);
          
    return NULL;
  }

  bool set_user_quota(uid_t uid, UserQuotaData &user_quota_data)
  {
    UserQuotaData *l = NULL;
    if (user_quota_map.count(uid)) {
      return false;
    }
      
    //dout(0) << "set_user_quota uid:" << uid << dendl;
    user_quota_map[uid] = user_quota_data;
    l = &user_quota_map[uid];

    is_change = true;
   
    return true;;
  }

  bool modify_user_quota(uid_t uid, UserQuotaData &user_quota_data)
  {
    if (!user_quota_map.count(uid)) {
      return false;
    }

    UserQuotaData *l = &user_quota_map[uid];
    l->modify(user_quota_data);
    is_change = true;

    return true;
  }

  UserQuotaData *update_user_quota(uid_t uid, UserQuotaData &user_quota_data)
  {
    UserQuotaData *l = NULL;
    if (user_quota_map.count(uid)) {
      l = &(user_quota_map[uid]);
      l->used_bytes += user_quota_data.buf_used;
    }

    is_change = true;
   
    return l;
  }

  void update_user_quota(map<uid_t, UserQuotaData> &update_user_quota_map);
  
  void del_user_quota(uid_t uid)
  {
    if (user_quota_map.count(uid))
      user_quota_map.erase(uid);

    is_change = true;
  }

  bool have_user_quota(uid_t uid)
  {
    if (user_quota_map.count(uid))
      return true;
    else
      return false;
  }

  void clear_user_quota(void)
  {
    user_quota_map.clear();
    is_change = true;
  }

  int get_user_quota_size()
  {
    return user_quota_map.size();
  }
};

class UserFileQuotaMgr : public QuotaMgrBase {
public:
  map<uid_t, UserFileQuota> user_file_quota_map; // 用户文件数map
  
  
public:
  UserFileQuotaMgr(MDCache *c, MDS *m, uint64_t v=0) : QuotaMgrBase(c, m, v) {}
  void store(MDSInternalContextBase *fin);
  void fetch(MDSInternalContextBase *fin);
  void _fetched(bufferlist& bl, Context *fin);

  void update_user_file_quota(uid_t uid, int32_t vary_cnt);
};

class GrpQuotaMgr : public QuotaMgrBase {
public:
  map<gid_t, GrpQuotaData> grp_quota_map;      // 用户配额map
  
public:
  GrpQuotaMgr(MDCache *c, MDS *m, uint64_t v=0) : QuotaMgrBase(c, m, v) {}
  void store(MDSInternalContextBase *fin);
  void fetch(MDSInternalContextBase *fin);
  void _fetched(bufferlist& bl, Context *fin);

  GrpQuotaData* get_grp_quota(gid_t gid)
  {
    if (grp_quota_map.count(gid))
      return &(grp_quota_map[gid]);
          
    return NULL;
  }

  bool set_grp_quota(gid_t gid, GrpQuotaData &gid_quota_data)
  {
    GrpQuotaData *l = NULL;
    if (grp_quota_map.count(gid)) {
      return false;
    }
      
    //dout(0) << "set_gid_quota gid:" << gid << dendl;
    grp_quota_map[gid] = gid_quota_data;
    l = &grp_quota_map[gid];

    is_change = true;
   
    return true;;
  }

  bool modify_grp_quota(gid_t gid, GrpQuotaData &gid_quota_data)
  {
    if (!grp_quota_map.count(gid)) {
      return false;
    }

    GrpQuotaData *l = &grp_quota_map[gid];
    l->modify(gid_quota_data);
    is_change = true;

    return true;
  }

  GrpQuotaData *update_grp_quota(gid_t gid, GrpQuotaData &gid_quota_data)
  {
    GrpQuotaData *l = NULL;
    if (grp_quota_map.count(gid)) {
      l = &(grp_quota_map[gid]);
      l->used_bytes += gid_quota_data.buf_used;
    }

    is_change = true;
   
    return l;
  }

  void update_grp_quota(map<gid_t, GrpQuotaData> &update_gid_quota_map);
  
  void del_grp_quota(gid_t gid)
  {
    if (grp_quota_map.count(gid))
      grp_quota_map.erase(gid);

    is_change = true;
  }

  bool have_grp_quota(gid_t gid)
  {
    if (grp_quota_map.count(gid))
      return true;
    else
      return false;
  }

  void clear_grp_quota(void)
  {
    grp_quota_map.clear();
	  is_change = true;
	}

  int get_grp_quota_size()
  {
    return grp_quota_map.size();
  }
};

class GrpFileQuotaMgr : public QuotaMgrBase {
public:
  map<gid_t, GrpFileQuota> grp_file_quota_map; // 用户文件数map
  
  
public:
  GrpFileQuotaMgr(MDCache *c, MDS *m, uint64_t v=0) : QuotaMgrBase(c, m, v) {}
  void store(MDSInternalContextBase *fin);
  void fetch(MDSInternalContextBase *fin);
  void _fetched(bufferlist& bl, Context *fin);

  void update_grp_file_quota(gid_t gid, int32_t vary_cnt);
};

class GrpUsedQuotaMgr : public QuotaMgrBase  {
public:
  map<gid_t, uint64_t> grp_used_quota_map; // 用户文件数map
  
  
public:
  GrpUsedQuotaMgr(MDCache *c, MDS *m, uint64_t v=0) : QuotaMgrBase(c, m, v) {}
  void store(MDSInternalContextBase *fin);
  void fetch(MDSInternalContextBase *fin);
  void _fetched(bufferlist& bl, Context *fin);

  void add(gid_t gid, uint64_t user_quota_size);
  void sub(gid_t gid, uint64_t user_quota_size);
  void modify(gid_t gid, uint64_t new_user_quota_size, uint64_t old_user_quota_size);
  uint64_t get_grp_used_quota(gid_t gid);
  void clear() {
    grp_used_quota_map.clear();
    is_change = true;
  }
};

class DirQuota : public QuotaMgrBase {
  public:
    map<inodeno_t, DirQuotaData> dir_quota_map;

  public:
    DirQuota(MDCache *c, MDS *m, uint64_t v=0) : QuotaMgrBase(c, m, v) {}
    void store(MDSInternalContextBase *fin);
    void fetch(MDSInternalContextBase *fin);
    void _fetched(bufferlist& bl, Context *fin);

    //get used bytes
    DirQuotaData* get_used_bytes(inodeno_t ino)
    {
      if (dir_quota_map.count(ino))
        return &(dir_quota_map[ino]);

      return NULL;
    }

    //update used_bytes
    DirQuotaData *update_dir_quota(inodeno_t ino, DirQuotaData &dqdata)
    {
      DirQuotaData *l = NULL;
      if (dir_quota_map.count(ino)) {
        l = &(dir_quota_map[ino]);
        l->used_bytes += dqdata.buf_incused;
				if( dqdata.buf_decused > l->used_bytes )
					l->used_bytes = 0;
				else 
					l->used_bytes -= dqdata.buf_decused;
      }

      is_change = true;

      return l;
    }

    void update_dir_quota(map<inodeno_t, DirQuotaData> &update_dir_quota_map)
    {
      for (map<inodeno_t, DirQuotaData>::iterator it = update_dir_quota_map.begin();
        it != update_dir_quota_map.end();
        ++it) {
        if (dir_quota_map.count(it->first)) {
          dir_quota_map[it->first].used_bytes += it->second.buf_incused;
					if( it->second.buf_decused > dir_quota_map[it->first].used_bytes )
            dir_quota_map[it->first].used_bytes = 0;
					else
						dir_quota_map[it->first].used_bytes -= it->second.buf_decused;
        }
      }

      is_change = true;
    }

    //create a new dir quota data
    bool set_dir_quota(inodeno_t ino, DirQuotaData &dqdata)
    {
      if (dir_quota_map.count(ino))
        return false;

      dir_quota_map[ino] = dqdata;

      is_change = true;
      return true;;
    }

    //delete
    void del_dir_quota(inodeno_t ino)
    {
      if (dir_quota_map.count(ino)) {
        dir_quota_map.erase(ino);
        is_change = true;
      }
    }

    //exist?
    bool have_dir_quota(inodeno_t ino)
    {
      if (dir_quota_map.count(ino))
        return true;
      else
        return false;
    }
};

#endif

