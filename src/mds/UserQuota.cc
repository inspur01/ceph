/******************************************************************************
 
                   版权所有 (C), 2015-2099, 浪潮电子信息产业股份有限公司
 
  ******************************************************************************
   文 件 名   : UserQuotaMgr.cpp
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
#include <errno.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <map>

#include "MDCache.h"
#include "MDS.h"
#include "UserQuota.h"

#include "Mutation.h"

#include "osdc/Journaler.h"
#include "osdc/Filer.h"

using namespace std;

extern struct ceph_file_layout g_default_file_layout;

#include "common/config.h"
#include "include/assert.h"

#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix _prefix(_dout)
static ostream& _prefix(std::ostream *_dout) {
  return *_dout  << "UserQuotaMgr ";
}


class CUserQuotaIOContext : public MDSIOContextBase
{
protected:
  UserQuotaMgr *user_quota;
  MDS *mds;
  MDS *get_mds() {return user_quota->mdcache->mds;}
public:
  CUserQuotaIOContext(UserQuotaMgr *uq) : user_quota(uq) {
    mds = user_quota->mdcache->mds;
    assert(uq != NULL);
  }
};


class C_IO_UserQuota_Stored : public CUserQuotaIOContext {
public:
  version_t version;
  Context *fin;
  C_IO_UserQuota_Stored(UserQuotaMgr *i, version_t v, Context *f) : CUserQuotaIOContext(i), version(v), fin(f) {}
  void finish(int r) {
    assert(r == 0);
    user_quota->set_change_ok();
    user_quota->set_used();
    
    dout(10) << "C_IO_UserQuota_Stored::finish version: " << user_quota->get_version() << dendl;
  }
};


class C_IO_UserQuota_Fetched : public CUserQuotaIOContext {
public:
  bufferlist bl;
  Context *fin;
  C_IO_UserQuota_Fetched(UserQuotaMgr *i, Context *f) : CUserQuotaIOContext(i), fin(f) {}
  void finish(int r) {
   // dout(1) << "C_IO_UserQuota_Fetched::finish "  << dendl;
    user_quota->_fetched(bl, fin);
  }
};

void UserQuotaMgr::store(MDSInternalContextBase *fin)
{
  //dout(1) << "UserQuotaMgr::store " << get_version() << dendl;
  if (is_nochange()) {
    return;
  }

  bool isused = true;
    
  // encode
  bufferlist bl;
  ::encode(user_quota_map, bl);
  ::encode(version, bl);
  ::encode(isused, bl);
  //dout(1) << "UserQuotaMgr::store len : " << bl.length() << dendl;
  
  // write it
  SnapContext snapc;
  ObjectOperation m;
  m.write_full(bl);

  object_t oid = CInode::get_object_name(ino, frag_t(), ".userquota");
  object_locator_t oloc(mdcache->mds->mdsmap->get_metadata_pool());

  Context *newfin =
    new C_OnFinisher(new C_IO_UserQuota_Stored(this, get_version(), fin),
		     &mdcache->mds->finisher);
  mdcache->mds->objecter->mutate(oid, oloc, m, snapc,
				 ceph_clock_now(g_ceph_context), 0,
				 NULL, newfin);
}


void UserQuotaMgr::fetch(MDSInternalContextBase *fin)
{
  //dout(1) << "UserQuotaMgr::fetch" << dendl;

  C_IO_UserQuota_Fetched *c = new C_IO_UserQuota_Fetched(this, fin);
  C_GatherBuilder gather(g_ceph_context, new C_OnFinisher(c, &mdcache->mds->finisher));

  object_locator_t oloc(mdcache->mds->mdsmap->get_metadata_pool());

  // read from separate object too
  object_t oid = CInode::get_object_name(ino, frag_t(), ".userquota");
  mdcache->mds->objecter->read(oid, oloc, 0, 0, CEPH_NOSNAP, &c->bl, 0, gather.new_sub());

  gather.activate();
}

void UserQuotaMgr::_fetched(bufferlist& bl, Context *fin)
{ 
  if (bl.length() <= 0) {
    return;
  }
  
  bufferlist::iterator p;
  
  p = bl.begin();

  ::decode(user_quota_map, p);
  ::decode(version, p);
  ::decode(is_used, p);

  fin->complete(0);
  
  dout(10) << "UserQuotaMgr::_fetched version : " << version << dendl;
}

void UserQuotaMgr::update_user_quota(map<uid_t, UserQuotaData> &update_user_quota_map)
{
  for (map<uid_t, UserQuotaData>::iterator it = update_user_quota_map.begin();
    it != update_user_quota_map.end();
    ++it) {
    if (user_quota_map.count(it->first)) {
      user_quota_map[it->first].used_bytes += it->second.buf_used;

      dout(10) << "UserFileQuotaMgr::update_user_quota uid: " << it->first << ", buf_used : " <<  it->second.buf_used << dendl;
    }
  }

  is_change = true;
}


class CUserFileQuotaIOContext : public MDSIOContextBase
{
protected:
  UserFileQuotaMgr *user_file_quota;
  MDS *mds;
  MDS *get_mds() {return user_file_quota->mdcache->mds;}
public:
  CUserFileQuotaIOContext(UserFileQuotaMgr *ufq) : user_file_quota(ufq) {
    mds = user_file_quota->mdcache->mds;
    assert(ufq != NULL);
  }
};

class C_IO_UserFileQuota_Stored : public CUserFileQuotaIOContext {
public:
  version_t version;
  Context *fin;
  C_IO_UserFileQuota_Stored(UserFileQuotaMgr *i, version_t v, Context *f) : CUserFileQuotaIOContext(i), version(v), fin(f) {}
  void finish(int r) {
    assert(r == 0);
    //user_file_quota->inc_version();
    user_file_quota->set_change_ok();
    user_file_quota->set_used();
    dout(10) << "C_IO_UserFileQuota_Stored::finish version: " << user_file_quota->get_version() << dendl;
  }
};

class C_IO_UserFileQuota_Fetched : public CUserFileQuotaIOContext {
public:
  bufferlist bl;
  Context *fin;
  C_IO_UserFileQuota_Fetched(UserFileQuotaMgr *i, Context *f) : CUserFileQuotaIOContext(i), fin(f) {}
  void finish(int r) {
   // dout(1) << "C_IO_UserFileQuota_Fetched::finish "  << dendl;
    user_file_quota->_fetched(bl, fin);
  }
};

void UserFileQuotaMgr::store(MDSInternalContextBase *fin)
{
  if (is_nochange()) {
    return;
  }

  bool isused = true;
  
  // encode
  bufferlist bl;
  ::encode(user_file_quota_map, bl);
  ::encode(version, bl);
  ::encode(isused, bl);
  
  // write it
  SnapContext snapc;
  ObjectOperation m;
  m.write_full(bl);

  object_t oid = CInode::get_object_name(ino, frag_t(), ".userfilequota");
  object_locator_t oloc(mdcache->mds->mdsmap->get_metadata_pool());

  Context *newfin =
    new C_OnFinisher(new C_IO_UserFileQuota_Stored(this, get_version(), fin),
		     &mdcache->mds->finisher);
  mdcache->mds->objecter->mutate(oid, oloc, m, snapc,
				 ceph_clock_now(g_ceph_context), 0,
				 NULL, newfin);
}


void UserFileQuotaMgr::fetch(MDSInternalContextBase *fin)
{
  C_IO_UserFileQuota_Fetched *c = new C_IO_UserFileQuota_Fetched(this, fin);
  C_GatherBuilder gather(g_ceph_context, new C_OnFinisher(c, &mdcache->mds->finisher));

  object_locator_t oloc(mdcache->mds->mdsmap->get_metadata_pool());

  // read from separate object too
  object_t oid = CInode::get_object_name(ino, frag_t(), ".userfilequota");
  mdcache->mds->objecter->read(oid, oloc, 0, 0, CEPH_NOSNAP, &c->bl, 0, gather.new_sub());

  gather.activate();
}

void UserFileQuotaMgr::_fetched(bufferlist& bl, Context *fin)
{
  if (bl.length() <= 0) {
    return;
  }

  dout(10) << "UserFileQuotaMgr::_fetched begin"  << dendl;
  
  bufferlist::iterator p;
  
  p = bl.begin();

  ::decode(user_file_quota_map, p);
  ::decode(version, p);
  ::decode(is_used, p);
    
  dout(10) << "UserFileQuotaMgr::_fetched end "  << dendl;
  fin->complete(0);
}

void UserFileQuotaMgr::update_user_file_quota(uid_t uid, int32_t vary_cnt)
{
  UserFileQuota *l = NULL;
  if (user_file_quota_map.count(uid)) {
    l = &(user_file_quota_map[uid]);
  }
  else {             
    user_file_quota_map[uid] =*(new UserFileQuota(uid));
    l = &(user_file_quota_map[uid]);
  }

  l->update(vary_cnt);

  is_change = true;
}

// Group Quota
class CGrpQuotaIOContext : public MDSIOContextBase
{
protected:
  GrpQuotaMgr *grp_quota;
  MDS *mds;
  MDS *get_mds() {return grp_quota->mdcache->mds;}
public:
  CGrpQuotaIOContext(GrpQuotaMgr *uq) : grp_quota(uq) {
    mds = grp_quota->mdcache->mds;
    assert(uq != NULL);
  }
};


class C_IO_GrpQuota_Stored : public CGrpQuotaIOContext {
public:
  version_t version;
  Context *fin;
  C_IO_GrpQuota_Stored(GrpQuotaMgr *i, version_t v, Context *f) : CGrpQuotaIOContext(i), version(v), fin(f) {}
  void finish(int r) {
    assert(r == 0);
    grp_quota->set_change_ok();
    grp_quota->set_used();
    
    dout(10) << "C_IO_GrpQuota_Stored::finish version: " << grp_quota->get_version() << dendl;
  }
};


class C_IO_GrpQuota_Fetched : public CGrpQuotaIOContext {
public:
  bufferlist bl;
  Context *fin;
  C_IO_GrpQuota_Fetched(GrpQuotaMgr *i, Context *f) : CGrpQuotaIOContext(i), fin(f) {}
  void finish(int r) {
   // dout(1) << "C_IO_GrpQuota_Fetched::finish "  << dendl;
    grp_quota->_fetched(bl, fin);
  }
};

void GrpQuotaMgr::store(MDSInternalContextBase *fin)
{
  //dout(1) << "GrpQuotaMgr::store " << get_version() << dendl;
  if (is_nochange()) {
    return;
  }

  bool isused = true;
    
  // encode
  bufferlist bl;
  ::encode(grp_quota_map, bl);
  ::encode(version, bl);
  ::encode(isused, bl);
  //dout(1) << "GrpQuotaMgr::store len : " << bl.length() << dendl;
  
  // write it
  SnapContext snapc;
  ObjectOperation m;
  m.write_full(bl);

  object_t oid = CInode::get_object_name(ino, frag_t(), ".grpquota");
  object_locator_t oloc(mdcache->mds->mdsmap->get_metadata_pool());

  Context *newfin =
    new C_OnFinisher(new C_IO_GrpQuota_Stored(this, get_version(), fin),
		     &mdcache->mds->finisher);
  mdcache->mds->objecter->mutate(oid, oloc, m, snapc,
				 ceph_clock_now(g_ceph_context), 0,
				 NULL, newfin);
}


void GrpQuotaMgr::fetch(MDSInternalContextBase *fin)
{
  //dout(1) << "GrpQuotaMgr::fetch" << dendl;

  C_IO_GrpQuota_Fetched *c = new C_IO_GrpQuota_Fetched(this, fin);
  C_GatherBuilder gather(g_ceph_context, new C_OnFinisher(c, &mdcache->mds->finisher));

  object_locator_t oloc(mdcache->mds->mdsmap->get_metadata_pool());

  // read from separate object too
  object_t oid = CInode::get_object_name(ino, frag_t(), ".grpquota");
  mdcache->mds->objecter->read(oid, oloc, 0, 0, CEPH_NOSNAP, &c->bl, 0, gather.new_sub());

  gather.activate();
}

void GrpQuotaMgr::_fetched(bufferlist& bl, Context *fin)
{ 
  if (bl.length() <= 0) {
    return;
  }
  
  bufferlist::iterator p;
  
  p = bl.begin();

  ::decode(grp_quota_map, p);
  ::decode(version, p);
  ::decode(is_used, p);

  fin->complete(0);
  
  dout(10) << "GrpQuotaMgr::_fetched version : " << version << dendl;
}

void GrpQuotaMgr::update_grp_quota(map<uid_t, GrpQuotaData> &update_grp_quota_map)
{
  for (map<uid_t, GrpQuotaData>::iterator it = update_grp_quota_map.begin();
    it != update_grp_quota_map.end();
    ++it) {
    if (grp_quota_map.count(it->first)) {
      grp_quota_map[it->first].used_bytes += it->second.buf_used;

      dout(10) << "GrpFileQuotaMgr::update_grp_quota uid: " << it->first << ", buf_used : " <<  it->second.buf_used << dendl;
    }
  }

  is_change = true;
}


class CGrpFileQuotaIOContext : public MDSIOContextBase
{
protected:
  GrpFileQuotaMgr *grp_file_quota;
  MDS *mds;
  MDS *get_mds() {return grp_file_quota->mdcache->mds;}
public:
  CGrpFileQuotaIOContext(GrpFileQuotaMgr *ufq) : grp_file_quota(ufq) {
    mds = grp_file_quota->mdcache->mds;
    assert(ufq != NULL);
  }
};

class C_IO_GrpFileQuota_Stored : public CGrpFileQuotaIOContext {
public:
  version_t version;
  Context *fin;
  C_IO_GrpFileQuota_Stored(GrpFileQuotaMgr *i, version_t v, Context *f) : CGrpFileQuotaIOContext(i), version(v), fin(f) {}
  void finish(int r) {
    assert(r == 0);
    //grp_file_quota->inc_version();
    grp_file_quota->set_change_ok();
    grp_file_quota->set_used();
    dout(10) << "C_IO_GrpFileQuota_Stored::finish version: " << grp_file_quota->get_version() << dendl;
  }
};

void GrpFileQuotaMgr::store(MDSInternalContextBase *fin)
{
  if (is_nochange()) {
    return;
  }

  bool isused = true;
  
  // encode
  bufferlist bl;
  ::encode(grp_file_quota_map, bl);
  ::encode(version, bl);
  ::encode(isused, bl);
  
  // write it
  SnapContext snapc;
  ObjectOperation m;
  m.write_full(bl);

  object_t oid = CInode::get_object_name(ino, frag_t(), ".grpfilequota");
  object_locator_t oloc(mdcache->mds->mdsmap->get_metadata_pool());

  Context *newfin =
    new C_OnFinisher(new C_IO_GrpFileQuota_Stored(this, get_version(), fin),
		     &mdcache->mds->finisher);
  mdcache->mds->objecter->mutate(oid, oloc, m, snapc,
				 ceph_clock_now(g_ceph_context), 0,
				 NULL, newfin);
}

class C_IO_GrpFileQuota_Fetched : public CGrpFileQuotaIOContext {
public:
  bufferlist bl;
  Context *fin;
  C_IO_GrpFileQuota_Fetched(GrpFileQuotaMgr *i, Context *f) : CGrpFileQuotaIOContext(i), fin(f) {}
  void finish(int r) {
   // dout(1) << "C_IO_GrpFileQuota_Fetched::finish "  << dendl;
    grp_file_quota->_fetched(bl, fin);
  }
};

void GrpFileQuotaMgr::fetch(MDSInternalContextBase *fin)
{
  C_IO_GrpFileQuota_Fetched *c = new C_IO_GrpFileQuota_Fetched(this, fin);
  C_GatherBuilder gather(g_ceph_context, new C_OnFinisher(c, &mdcache->mds->finisher));

  object_locator_t oloc(mdcache->mds->mdsmap->get_metadata_pool());

  // read from separate object too
  object_t oid = CInode::get_object_name(ino, frag_t(), ".grpfilequota");
  mdcache->mds->objecter->read(oid, oloc, 0, 0, CEPH_NOSNAP, &c->bl, 0, gather.new_sub());

  gather.activate();
}

void GrpFileQuotaMgr::_fetched(bufferlist& bl, Context *fin)
{
  if (bl.length() <= 0) {
    return;
  }

  dout(10) << "GrpFileQuotaMgr::_fetched begin"  << dendl;
  
  bufferlist::iterator p;
  
  p = bl.begin();

  ::decode(grp_file_quota_map, p);
  ::decode(version, p);
  ::decode(is_used, p);
    
  dout(10) << "GrpFileQuotaMgr::_fetched end "  << dendl;
  fin->complete(0);
}

void GrpFileQuotaMgr::update_grp_file_quota(gid_t gid, int32_t vary_cnt)
{
  GrpFileQuota *l = NULL;
  if (grp_file_quota_map.count(gid)) {
    l = &(grp_file_quota_map[gid]);
  }
  else {             
    grp_file_quota_map[gid] =*(new GrpFileQuota(gid));
    l = &(grp_file_quota_map[gid]);
  }

  l->update(vary_cnt);

  is_change = true;
}

// Group Quota end

// 
class CGrpUsedQuotaIOContext : public MDSIOContextBase
{
protected:
  GrpUsedQuotaMgr *grp_used_quota;
  MDS *mds;
  MDS *get_mds() {return grp_used_quota->mdcache->mds;}
public:
  CGrpUsedQuotaIOContext(GrpUsedQuotaMgr *guq) : grp_used_quota(guq) {
    assert(guq != NULL);
  }
};

class C_IO_GrpUsedQuota_Stored : public CGrpUsedQuotaIOContext {
public:
  version_t version;
  Context *fin;
  C_IO_GrpUsedQuota_Stored(GrpUsedQuotaMgr *i, version_t v, Context *f) : CGrpUsedQuotaIOContext(i), version(v), fin(f) {}
  void finish(int r) {
    assert(r == 0);
    grp_used_quota->set_change_ok();
    grp_used_quota->set_used();
    dout(10) << "C_IO_GrpUsedQuota_Stored::finish version: " << grp_used_quota->get_version() << dendl;
  }
};

void GrpUsedQuotaMgr::store(MDSInternalContextBase *fin)
{
  if (is_nochange()) {
    return;
  }

  bool isused = true;
  
  // encode
  bufferlist bl;
  ::encode(grp_used_quota_map, bl);
  ::encode(version, bl);
  ::encode(isused, bl);
  
  // write it
  SnapContext snapc;
  ObjectOperation m;
  m.write_full(bl);

  object_t oid = CInode::get_object_name(ino, frag_t(), ".grpusedquota");
  object_locator_t oloc(mdcache->mds->mdsmap->get_metadata_pool());

  Context *newfin =
    new C_OnFinisher(new C_IO_GrpUsedQuota_Stored(this, get_version(), fin),
		     &mdcache->mds->finisher);
  mdcache->mds->objecter->mutate(oid, oloc, m, snapc,
				 ceph_clock_now(g_ceph_context), 0,
				 NULL, newfin);
}

class C_IO_GrpUsedQuota_Fetched : public CGrpUsedQuotaIOContext {
public:
  bufferlist bl;
  Context *fin;
  C_IO_GrpUsedQuota_Fetched(GrpUsedQuotaMgr *i, Context *f) : CGrpUsedQuotaIOContext(i), fin(f) {}
  void finish(int r) {
   // dout(1) << "C_IO_GrpFileQuota_Fetched::finish "  << dendl;
    grp_used_quota->_fetched(bl, fin);
  }
};

void GrpUsedQuotaMgr::fetch(MDSInternalContextBase *fin)
{
  C_IO_GrpUsedQuota_Fetched *c = new C_IO_GrpUsedQuota_Fetched(this, fin);
  C_GatherBuilder gather(g_ceph_context, new C_OnFinisher(c, &mdcache->mds->finisher));

  object_locator_t oloc(mdcache->mds->mdsmap->get_metadata_pool());

  // read from separate object too
  object_t oid = CInode::get_object_name(ino, frag_t(), ".grpusedquota");
  mdcache->mds->objecter->read(oid, oloc, 0, 0, CEPH_NOSNAP, &c->bl, 0, gather.new_sub());

  gather.activate();
}

void GrpUsedQuotaMgr::_fetched(bufferlist& bl, Context *fin)
{
  if (bl.length() <= 0) {
    return;
  }

  dout(10) << "GrpUsedQuotaMgr::_fetched begin"  << dendl;
  
  bufferlist::iterator p;
  
  p = bl.begin();

  ::decode(grp_used_quota_map, p);
  ::decode(version, p);
  ::decode(is_used, p);

  dout(10) << "GrpUsedQuotaMgr::_fetched end "  << dendl;
  fin->complete(0);
}

void GrpUsedQuotaMgr::add(gid_t gid, uint64_t user_quota_size)
{
  grp_used_quota_map[gid] += user_quota_size;
  is_change = true;
}

void GrpUsedQuotaMgr::sub(gid_t gid, uint64_t user_quota_size)
{
  grp_used_quota_map[gid] -= user_quota_size;
  is_change = true;
}

void GrpUsedQuotaMgr::modify(gid_t gid, uint64_t new_user_quota_size, uint64_t old_user_quota_size)
{
  grp_used_quota_map[gid] += new_user_quota_size;
  grp_used_quota_map[gid] -= old_user_quota_size;
  is_change = true;
}


uint64_t GrpUsedQuotaMgr::get_grp_used_quota(gid_t gid)
{
  if (!grp_used_quota_map.count(gid)) {
    grp_used_quota_map[gid] = 0;
  }

  return grp_used_quota_map[gid];
}

class CDirQuotaIOContext : public MDSIOContextBase
{
protected:
  DirQuota *dir_quota;
  MDS *mds;
  MDS *get_mds() {return dir_quota->mdcache->mds;}
public:
  CDirQuotaIOContext(DirQuota *dq) : dir_quota(dq) {
    mds = dir_quota->mdcache->mds;
    assert(dq != NULL);
  }
};

class C_IO_DirQuota_Stored : public CDirQuotaIOContext {
public:
  version_t version;
  Context *fin;
  C_IO_DirQuota_Stored(DirQuota *i, version_t v, Context *f) : CDirQuotaIOContext(i), version(v), fin(f) {}
  void finish(int r) {
    assert(r == 0);
    dir_quota->set_change_ok();
    dir_quota->set_used();
    
    dout(10) << "C_IO_UserQuota_Stored::finish version: " << dir_quota->get_version() << dendl;
  }
};

class C_IO_DirQuota_Fetched : public CDirQuotaIOContext {
public:
  bufferlist bl;
  Context *fin;
  C_IO_DirQuota_Fetched(DirQuota *i, Context *f) : CDirQuotaIOContext(i), fin(f) {}
  void finish(int r) {
    dir_quota->_fetched(bl, fin);
  }
};

void DirQuota::store(MDSInternalContextBase *fin)
{
  if (is_nochange())
    return;

  bool isused = true;
  
  // encode
  bufferlist bl;
  ::encode(dir_quota_map, bl);
  ::encode(version, bl);
  ::encode(isused, bl);

  // write it
  SnapContext snapc;
  ObjectOperation m;
  m.write_full(bl);

  object_t oid = CInode::get_object_name(ino, frag_t(), ".dirquota");
  object_locator_t oloc(mdcache->mds->mdsmap->get_metadata_pool());

  Context *newfin =
    new C_OnFinisher(new C_IO_DirQuota_Stored(this, get_version(), fin),
	  &mdcache->mds->finisher);
  mdcache->mds->objecter->mutate(oid, oloc, m, snapc,
		ceph_clock_now(g_ceph_context), 0, NULL, newfin);
}

void DirQuota::fetch(MDSInternalContextBase *fin)
{
  C_IO_DirQuota_Fetched *c = new C_IO_DirQuota_Fetched(this, fin);
  C_GatherBuilder gather(g_ceph_context, new C_OnFinisher(c, &mdcache->mds->finisher));

  object_locator_t oloc(mdcache->mds->mdsmap->get_metadata_pool());

  // read from separate object too
  object_t oid = CInode::get_object_name(ino, frag_t(), ".dirquota");
  mdcache->mds->objecter->read(oid, oloc, 0, 0, CEPH_NOSNAP, &c->bl, 0, gather.new_sub());

  gather.activate();
}

void DirQuota::_fetched(bufferlist& bl, Context *fin)
{ 
  if (bl.length() <= 0)
    return;

  bufferlist::iterator p;

  p = bl.begin();

  ::decode(dir_quota_map, p);
  ::decode(version, p);
  ::decode(is_used, p);

  fin->complete(0);
  
  dout(10) << "DirQuota::_fetched version : " << version << dendl;
}

