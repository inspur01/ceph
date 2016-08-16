// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */


#ifndef CEPH_CLIENT_H
#define CEPH_CLIENT_H

#include "include/types.h"

// stl
#include <string>
#include <memory>
#include <set>
#include <map>
#include <fstream>
#include <exception>
using std::set;
using std::map;
using std::fstream;

#include "posix_acl.h"
#include "posix_new_acl.h"

#include "include/unordered_map.h"
#include "include/filepath.h"
#include "include/interval_set.h"
#include "include/lru.h"

//#include "barrier.h"


#include "mds/mdstypes.h"

#include "msg/Message.h"
#include "msg/Dispatcher.h"
#include "msg/Messenger.h"

#include "common/Mutex.h"
#include "common/Timer.h"
#include "common/Finisher.h"

#include "common/compiler_extensions.h"
#include "common/cmdparse.h"
#include "common/UserQuotaData.h"
#include "common/UserFileQuota.h"
#include "common/GrpQuotaData.h"
#include "common/GrpFileQuota.h"
#include "common/DirQuotaData.h"

#include "osdc/FileCacher.h"

#include "UserGroups.h"


using  posix::PosixAcl;

class MDSMap;
class MonClient;

class CephContext;
class MClientReply;
class MClientRequest;
class MClientSession;
class MClientRequest;
class MClientRequestForward;
struct MClientLease;
class MClientCaps;
class MClientCapRelease;
struct MClientDirQuota;

struct DirStat;
struct LeaseStat;
struct InodeStat;

class Filer;
class Objecter;
class WritebackHandler;

class PerfCounters;

enum {
  l_c_first = 20000,
  l_c_reply,
  l_c_lat,
  l_c_owrlat,
  l_c_ordlat,
  l_c_wrlat,
  l_c_last,
};



// ============================================
// types for my local metadata cache
/* basic structure:

 - Dentries live in an LRU loop.  they get expired based on last access.
      see include/lru.h.  items can be bumped to "mid" or "top" of list, etc.
 - Inode has ref count for each Fh, Dir, or Dentry that points to it.
 - when Inode ref goes to 0, it's expired.
 - when Dir is empty, it's removed (and it's Inode ref--)

*/

/* getdir result */
struct DirEntry {
  string d_name;
  struct stat st;
  int stmask;
  DirEntry(const string &s) : d_name(s), stmask(0) {}
  DirEntry(const string &n, struct stat& s, int stm) : d_name(n), st(s), stmask(stm) {}
};

struct Inode;
struct Cap;
class Dir;
class Dentry;
struct SnapRealm;
struct Fh;
struct CapSnap;

struct MetaSession;
struct MetaRequest;
class ceph_lock_state_t;

typedef void (*client_ino_callback_t)(void *handle, vinodeno_t ino, int64_t off, int64_t len);

typedef void (*client_dentry_callback_t)(void *handle, vinodeno_t dirino,
    vinodeno_t ino, string& name);
typedef void (*client_remount_callback_t)(void *handle);

typedef int (*client_getgroups_callback_t)(void *handle, uid_t uid, gid_t **sgids);

typedef void(*client_switch_interrupt_callback_t)(void *req, void *data);

struct client_callback_args {
  void *handle;
  client_ino_callback_t ino_cb;
  client_dentry_callback_t dentry_cb;
  client_remount_callback_t remount_cb;
  client_getgroups_callback_t getgroups_cb;
};

// ========================================================
// client interface

struct dir_result_t {
  static const int SHIFT = 28;
  static const int64_t MASK = (1 << SHIFT) - 1;
  static const loff_t END = 1ULL << (SHIFT + 32);

  static uint64_t make_fpos(unsigned frag, unsigned off)
  {
    return ((uint64_t)frag << SHIFT) | (uint64_t)off;
  }
  static unsigned fpos_frag(uint64_t p)
  {
    return (p & ~END) >> SHIFT;
  }
  static unsigned fpos_off(uint64_t p)
  {
    return p & MASK;
  }


  Inode *inode;

  int64_t offset;        // high bits: frag_t, low bits: an offset

  uint64_t this_offset;  // offset of last chunk, adjusted for . and ..
  uint64_t next_offset;  // offset of next chunk (last_name's + 1)
  string last_name;      // last entry in previous chunk

  uint64_t release_count;
  uint64_t ordered_count;
  int start_shared_gen;  // dir shared_gen at start of readdir

  frag_t buffer_frag;
  vector<pair<string,Inode*> > *buffer;

  string at_cache_name;  // last entry we successfully returned

  dir_result_t(Inode *in);

  frag_t frag()
  {
    return frag_t(offset >> SHIFT);
  }
  unsigned fragpos()
  {
    return offset & MASK;
  }

  void next_frag()
  {
    frag_t fg = offset >> SHIFT;
    if (fg.is_rightmost())
      set_end();
    else
      set_frag(fg.next());
  }
  void set_frag(frag_t f)
  {
    offset = (uint64_t)f << SHIFT;
    assert(sizeof(offset) == 8);
  }
  void set_end()
  {
    offset |= END;
  }
  bool at_end()
  {
    return (offset & END);
  }

  void reset()
  {
    last_name.clear();
    at_cache_name.clear();
    next_offset = 2;
    this_offset = 0;
    offset = 0;
    delete buffer;
    buffer = 0;
  }
};

class Client : public Dispatcher
{
public:
  CephContext *cct;

  PerfCounters *logger;
  PosixAcl *pacl;//add by li.jiebj
  class CommandHook : public AdminSocketHook
  {
    Client *m_client;
  public:
    CommandHook(Client *client);
    bool call(std::string command, cmdmap_t &cmdmap, std::string format,
              bufferlist& out);
  };
  CommandHook m_command_hook;

  // cluster descriptors
  MDSMap *mdsmap;

  SafeTimer timer;

  client_switch_interrupt_callback_t switch_interrupt_cb;

  void *callback_handle;
  client_remount_callback_t remount_cb;
  client_ino_callback_t ino_invalidate_cb;
  client_dentry_callback_t dentry_invalidate_cb;
  client_getgroups_callback_t getgroups_cb;

  Finisher async_ino_invalidator;
  Finisher async_dentry_invalidator;
  Finisher remount_finisher;
  Finisher objecter_finisher;
  Finisher interrupt_finisher;

  Context *tick_event;
  utime_t last_cap_renew;
  void renew_caps();
  void renew_caps(MetaSession *session);
  void flush_cap_releases();
public:
  void tick();

protected:
  MonClient *monclient;
  Messenger *messenger;
  client_t whoami;

  int user_id;
  int group_id;
  int acl_type;

  int get_uid() {
    if (user_id > 0)
      return user_id;
    return ::geteuid();
  }

  int get_gid() {
    if (group_id > 0)
        return group_id;
    return ::getegid();
  }

  set<int> extra_root_users;
  int get_uid(string username);
  void parse_extra_root_users();
  bool check_if_root_user(int uid);

  // mds sessions
  map<int, MetaSession*> mds_sessions;  // mds -> push seq
  list<Cond*> waiting_for_mdsmap;

  void get_session_metadata(std::map<std::string, std::string> *meta) const;
  bool have_open_session(int mds);
  void got_mds_push(MetaSession *s);
  MetaSession *_get_mds_session(int mds, Connection *con);  ///< return session for mds *and* con; null otherwise
  MetaSession *_get_or_open_mds_session(int mds);
  MetaSession *_open_mds_session(int mds);
  void _close_mds_session(MetaSession *s);
  void _closed_mds_session(MetaSession *s);
  void _kick_stale_sessions();
  void handle_client_session(MClientSession *m);
  void send_reconnect(MetaSession *s);
  void resend_unsafe_requests(MetaSession *s);

  // mds requests
  ceph_tid_t last_tid, last_flush_seq;
  map<ceph_tid_t, MetaRequest*> mds_requests;
  set<int>                 failed_mds;

  void dump_mds_requests(Formatter *f);
  void dump_mds_sessions(Formatter *f);

  int make_request(MetaRequest *req, int uid, int gid,
                   //MClientRequest *req, int uid, int gid,
                   Inode **ptarget = 0, bool *pcreated = 0,
                   int use_mds=-1, bufferlist *pdirbl=0);
  
  int make_request_asyn(MetaRequest *req, int uid, int gid,
                   //MClientRequest *req, int uid, int gid,
                   Inode **ptarget = 0, bool *pcreated = 0,
                   int use_mds=-1, bufferlist *pdirbl=0);
  void md_readahead_end(MetaRequest *request, MetaSession *session);
  void put_request(MetaRequest *request);

  int verify_reply_trace(int r, MetaRequest *request, MClientReply *reply,
                         Inode **ptarget, bool *pcreated, int uid, int gid);
  void encode_cap_releases(MetaRequest *request, int mds);
  int encode_inode_release(Inode *in, MetaRequest *req,
                           int mds, int drop,
                           int unless,int force=0);
  void encode_dentry_release(Dentry *dn, MetaRequest *req,
                             int mds, int drop, int unless);
  int choose_target_mds(MetaRequest *req);
  void connect_mds_targets(int mds);
  void send_request(MetaRequest *request, MetaSession *session);
  MClientRequest *build_client_request(MetaRequest *request);
  void kick_requests(MetaSession *session);
  void kick_requests_closed(MetaSession *session);
  void handle_client_request_forward(MClientRequestForward *reply);
  void handle_client_reply(MClientReply *reply);

  bool   initialized;
  bool   mounted;
  bool   unmounting;

  int local_osd;
  epoch_t local_osd_epoch;

  int unsafe_sync_write;

public:
  entity_name_t get_myname()
  {
    return messenger->get_myname();
  }
  void sync_write_commit(Inode *in);

protected:
  Filer                 *filer;
  ObjectCacher          *objectcacher;
  Objecter              *objecter;     // (non-blocking) osd interface
  WritebackHandler      *writeback_handler;

  // cache
  ceph::unordered_map<vinodeno_t, Inode*> inode_map;
  Inode*                 root;
  map<Inode*, Inode*>    root_parents;
  Inode*                 root_ancestor;
  LRU                    lru;    // lru list of Dentry's in our local metadata cache.

  /* D????tD∩車??‘那1車?米?listo赤o‘那y */
  xlist<Fh *> delay_release; 
  void flush_delay_release();

  // all inodes with caps sit on either cap_list or delayed_caps.
  xlist<Inode*> delayed_caps, cap_list;
  int num_flushing_caps;
  ceph::unordered_map<inodeno_t,SnapRealm*> snap_realms;

  // Optional extra metadata about me to send to the MDS
  std::map<std::string, std::string> metadata;
  void populate_metadata();


  /* async block write barrier support */
  //map<uint64_t, BarrierContext* > barriers;

  SnapRealm *get_snap_realm(inodeno_t r);
  SnapRealm *get_snap_realm_maybe(inodeno_t r);
  void put_snap_realm(SnapRealm *realm);
  bool adjust_realm_parent(SnapRealm *realm, inodeno_t parent);
  inodeno_t update_snap_trace(bufferlist& bl, bool must_flush=true);
  inodeno_t _update_snap_trace(vector<SnapRealmInfo>& trace);
  void invalidate_snaprealm_and_children(SnapRealm *realm);

  Inode *open_snapdir(Inode *diri);

  void file_readahead(Inode *in);
  void file_readahead_ino(Inode *parent, Inode *cur_in);
  void file_readahead_time(Inode *parent, Inode *cur_in);

  // file handles, etc.
  interval_set<int> free_fd_set;  // unused fds
  ceph::unordered_map<int, Fh*> fd_map;

  int get_fd()
  {
    int fd = free_fd_set.range_start();
    free_fd_set.erase(fd, 1);
    return fd;
  }
  void put_fd(int fd)
  {
    free_fd_set.insert(fd, 1);
  }

  /*
   * Resolve file descriptor, or return NULL.
   */
  Fh *get_filehandle(int fd)
  {
    ceph::unordered_map<int, Fh*>::iterator p = fd_map.find(fd);
    if (p == fd_map.end())
      return NULL;
    return p->second;
  }

  // global client lock
  //  - protects Client and buffer cache both!
  Mutex                  client_lock;

  // helpers
  void wake_inode_waiters(MetaSession *s);
  void wait_on_list(list<Cond*>& ls);
  void signal_cond_list(list<Cond*>& ls);

  void wait_on_context_list(list<Context*>& ls);
  void signal_context_list(list<Context*>& ls);

  // -- metadata cache stuff

  // decrease inode ref.  delete if dangling.
  void put_inode(Inode *in, int n=1);
  void close_dir(Dir *dir);

  friend class C_Client_PutInode; // calls put_inode()
  friend class C_Client_CacheInvalidate;  // calls ino_invalidate_cb
  friend class C_Client_DentryInvalidate;  // calls dentry_invalidate_cb
  friend class C_Block_Sync; // Calls block map and protected helpers
  friend class C_C_Tick; // Asserts on client_lock
  friend class C_Client_SyncCommit; // Asserts on client_lock
  friend class C_Client_RequestInterrupt;

  //int get_cache_size() { return lru.lru_get_size(); }
  //void set_cache_size(int m) { lru.lru_set_max(m); }

  /**
   * Don't call this with in==NULL, use get_or_create for that
   * leave dn set to default NULL unless you're trying to add
   * a new inode to a pre-created Dentry
   */
  Dentry* link(Dir *dir, const string& name, Inode *in, Dentry *dn);
  void unlink(Dentry *dn, bool keepdir, bool keepdentry);

  // path traversal for high-level interface
  Inode *cwd;
  int path_walk(const filepath& fp, Inode **end, bool followsym=true, int uid = -1, int gid = -1);
  int fill_stat(Inode *in, struct stat *st, frag_info_t *dirstat=0, nest_info_t *rstat=0);
  void touch_dn(Dentry *dn);

  // trim cache.
  void trim_cache();
  void trim_cache_for_reconnect(MetaSession *s);
  void trim_dentry(Dentry *dn);
  void trim_caps(MetaSession *s, int max);
  void _invalidate_kernel_dcache();

  void dump_inode(Formatter *f, Inode *in, set<Inode*>& did, bool disconnected);
  void dump_cache(Formatter *f);  // debug

  // trace generation
  ofstream traceout;


  Cond mount_cond, sync_cond;


  // friends
  friend class SyntheticClient;
  bool ms_dispatch(Message *m);

  void ms_handle_connect(Connection *con);
  bool ms_handle_reset(Connection *con);
  void ms_handle_remote_reset(Connection *con);
  bool ms_get_authorizer(int dest_type, AuthAuthorizer **authorizer, bool force_new);
  void put_qtree(Inode *in);
  void invalidate_quota_tree(Inode *in);
  Inode* get_quota_root(Inode *in);
  bool is_quota_files_exceeded(Inode *in);
  bool is_quota_bytes_exceeded(Inode *in);
  bool is_quota_bytes_approaching(Inode *in);
  int stat_dir_bytes(Inode *in, uint64_t *plbytes);
  int md_readahead(Inode *in);
  int md_readahead_dir_complete(Inode *in);
public:
  void set_filer_flags(int flags);
  void clear_filer_flags(int flags);

  Client(Messenger *m, MonClient *mc);
  ~Client();
  void tear_down_cache();

  void update_metadata(std::string const &k, std::string const &v);

  client_t get_nodeid()
  {
    return whoami;
  }

  inodeno_t get_root_ino();
  Inode *get_root();

  int init()  WARN_UNUSED_RESULT;
  void shutdown();

  // messaging
  void handle_mds_map(class MMDSMap *m);

  void handle_lease(MClientLease *m);

  // inline data
  int uninline_data(Inode *in, Context *onfinish);

  // file caps
  void check_cap_issue(Inode *in, Cap *cap, unsigned issued);
  void add_update_cap(Inode *in, MetaSession *session, uint64_t cap_id,
                      unsigned issued, unsigned seq, unsigned mseq, inodeno_t realm,
                      int flags);
  void remove_cap(Cap *cap, bool queue_release);
  void remove_all_caps(Inode *in);
  void remove_session_caps(MetaSession *session);
  void mark_caps_dirty(Inode *in, int caps);
  int mark_caps_flushing(Inode *in);
  void flush_caps();
  void flush_caps(Inode *in, MetaSession *session);
  void kick_flushing_caps(MetaSession *session);
  void kick_maxsize_requests(MetaSession *session);
  int get_caps(Inode *in, int need, int want, int *have, loff_t endoff);
  int get_caps_used(Inode *in);

  void maybe_update_snaprealm(SnapRealm *realm, snapid_t snap_created, snapid_t snap_highwater,
                              vector<snapid_t>& snaps);

  void handle_snap(struct MClientSnap *m);
  void handle_caps(class MClientCaps *m);
  void handle_quota(struct MClientQuota *m);
  //user quota
  void handle_user_quota_broadcast(struct MClientUserQuota *m);
  void handle_set_quota_broadcast(UserQuotaData &qt);
  void handle_del_quota_broadcast(const UserQuotaData &qt);
  void handle_clear_quota_broadcast(void);
  void handle_user_file_quota_broadcast(struct MClientUserFileQuota * m);
  //group quota
  void handle_group_quota_broadcast(struct MClientGrpQuota *m);
  void handle_set_grp_quota_broadcast(GrpQuotaData &qt);
  void handle_del_grp_quota_broadcast(const GrpQuotaData &qt);
  void handle_clear_grp_quota_broadcast(void);
  void handle_group_file_quota_broadcast(struct MClientGrpFileQuota * m);
  
  void handle_cap_import(MetaSession *session, Inode *in, class MClientCaps *m);
  void handle_cap_export(MetaSession *session, Inode *in, class MClientCaps *m);
  void handle_cap_trunc(MetaSession *session, Inode *in, class MClientCaps *m);
  void handle_cap_flush_ack(MetaSession *session, Inode *in, Cap *cap, class MClientCaps *m);
  void handle_cap_flushsnap_ack(MetaSession *session, Inode *in, class MClientCaps *m);
  void handle_cap_grant(MetaSession *session, Inode *in, Cap *cap, class MClientCaps *m);
  void cap_delay_requeue(Inode *in);
  void send_cap(Inode *in, MetaSession *session, Cap *cap,
                int used, int want, int retain, int flush);
  void check_caps(Inode *in, bool is_delayed);
  void get_cap_ref(Inode *in, int cap);
  void put_cap_ref(Inode *in, int cap);
  void flush_snaps(Inode *in, bool all_again=false, CapSnap *again=0);
  void wait_sync_caps(uint64_t want);
  void queue_cap_snap(Inode *in, snapid_t seq=0);
  void finish_cap_snap(Inode *in, CapSnap *capsnap, int used);
  void _flushed_cap_snap(Inode *in, snapid_t seq);

  void _schedule_invalidate_dentry_callback(Dentry *dn, bool del);
  void _async_dentry_invalidate(vinodeno_t dirino, vinodeno_t ino, string& name);
  void _invalidate_inode_parents(Inode *in);

  void _schedule_invalidate_callback(Inode *in, int64_t off, int64_t len, bool keep_caps);
  void _invalidate_inode_cache(Inode *in);
  void _invalidate_inode_cache(Inode *in, int64_t off, int64_t len);
  void _async_invalidate(Inode *in, int64_t off, int64_t len, bool keep_caps);
  void _release(Inode *in);

  /**
   * Initiate a flush of the data associated with the given inode.
   * If you specify a Context, you are responsible for holding an inode
   * reference for the duration of the flush. If not, _flush() will
   * take the reference for you.
   * @param in The Inode whose data you wish to flush.
   * @param c The Context you wish us to complete once the data is
   * flushed. If already flushed, this will be called in-line.
   *
   * @returns true if the data was already flushed, false otherwise.
   */
  bool _flush(Inode *in, Context *c=NULL);
  void _flush_range(Inode *in, int64_t off, uint64_t size);
  void _flushed(Inode *in);
  void flush_set_callback(ObjectCacher::ObjectSet *oset);

  void close_release(Inode *in);
  void close_safe(Inode *in);

  void lock_fh_pos(Fh *f);
  void unlock_fh_pos(Fh *f);

  // metadata cache
  void update_dir_dist(Inode *in, DirStat *st);

  void insert_readdir_results(MetaRequest *request, MetaSession *session, Inode *diri);
  void insert_readahead_results(MetaRequest *request, MetaSession *session, Inode *diri);
  Inode* insert_trace(MetaRequest *request, MetaSession *session);
  void update_inode_file_bits(Inode *in,
                              uint64_t truncate_seq, uint64_t truncate_size, uint64_t size,
                              uint64_t time_warp_seq, utime_t ctime, utime_t mtime, utime_t atime,
                              version_t inline_version, bufferlist& inline_data,
                              int issued);
  Inode *add_update_inode(InodeStat *st, utime_t ttl, MetaSession *session);
  Dentry *insert_dentry_inode(Dir *dir, const string& dname, LeaseStat *dlease,
                              Inode *in, utime_t from, MetaSession *session,
                              Dentry *old_dentry = NULL);
  void update_dentry_lease(Dentry *dn, LeaseStat *dlease, utime_t from, MetaSession *session);


  // ----------------------
  // fs ops.
private:

  void fill_dirent(struct dirent *de, const char *name, int type, uint64_t ino, loff_t next_off);

  // some readdir helpers
  typedef int (*add_dirent_cb_t)(void *p, struct dirent *de, struct stat *st, int stmask, off_t off);

  int _opendir(Inode *in, dir_result_t **dirpp, int uid=-1, int gid=-1);
  void _readdir_drop_dirp_buffer(dir_result_t *dirp);
  bool _readdir_have_frag(dir_result_t *dirp);
  void _readdir_next_frag(dir_result_t *dirp);
  void _readdir_rechoose_frag(dir_result_t *dirp);
  int _readdir_get_frag(dir_result_t *dirp);
  int _readdir_cache_cb(dir_result_t *dirp, add_dirent_cb_t cb, void *p);
  void _closedir(dir_result_t *dirp);

  // other helpers
  void _fragmap_remove_non_leaves(Inode *in);

  void _ll_get(Inode *in);
  int _ll_put(Inode *in, int num);
  void _ll_drop_pins();

  Fh *_create_fh(Inode *in, int flags, int cmode);
  int _release_fh(Fh *fh);


  struct C_Readahead : public Context {
    Client *client;
    Inode *inode;
    C_Readahead(Client *c, Inode *i)
      : client(c),
        inode(i) { }
    void finish(int r)
    {
      //lsubdout(client->cct, client, 20) << "C_Readahead on " << inode << dendl;
      client->put_cap_ref(inode, CEPH_CAP_FILE_RD | CEPH_CAP_FILE_CACHE);
    }
  };

  int _read_sync(Fh *f, uint64_t off, uint64_t len, bufferlist *bl, bool *checkeof);
  int _read_async(Fh *f, uint64_t off, uint64_t len, bufferlist *bl);

  // internal interface
  //   call these with client_lock held!
  int _do_lookup(Inode *dir, const string& name, Inode **target, int uid, int gid);
  int _lookup(Inode *dir, const string& dname, Inode **target, int uid = 0, int gid = 0);
  /* add by zhd */
  int _lookup_for_create(Inode *dir, const string& dname, Inode **target);
  /* end add */
  int _link(Inode *in, Inode *dir, const char *name, int uid=-1, int gid=-1, Inode **inp = 0);
  int _unlink(Inode *dir, const char *name, int uid=-1, int gid=-1);
  int _rename(Inode *olddir, const char *oname, Inode *ndir, const char *nname, int uid=-1, int gid=-1);
  int _mkdir(Inode *dir, const char *name, mode_t mode, int uid=-1, int gid=-1, Inode **inp = 0);
  int _rmdir(Inode *dir, const char *name, int uid=-1, int gid=-1);
  int _symlink(Inode *dir, const char *name, const char *target, int uid=-1, int gid=-1, Inode **inp = 0);
  int _mknod(Inode *dir, const char *name, mode_t mode, dev_t rdev, int uid=-1, int gid=-1, Inode **inp = 0);
  int _setattr(Inode *in, struct stat *attr, int mask, int uid=-1, int gid=-1, Inode **inp = 0);
  int _getattr(Inode *in, int mask, int uid=-1, int gid=-1, bool force=false);
  int _readlink(Inode *in, char *buf, size_t size);
  int _getxattr(Inode *in, const char *name, void *value, size_t len, int uid=-1, int gid=-1);
  int _listxattr(Inode *in, char *names, size_t len, int uid=-1, int gid=-1);
  int _setxattr(Inode *in, const char *name, const void *value, size_t len, int flags, int uid=-1, int gid=-1);
  /*user quota*/
  int get_uid(string  usrstr, uid_t & uid,gid_t &gid);
  int get_gid(string  usrstr, gid_t &gid);
  int _setxattr_quota(const char *name, const void *value, size_t size, int flags);//lhd setquota
  int _set_quota_attr(const char *name, const void *value, size_t size, string op); //lhd set usrquota
  int _del_quota_attr(const char *name, const void *value, size_t size, string op); //lhd delete usrquota    
  int _get_quota_attr(const char *name,  void *value, size_t size); //lhd get userquota   
  int _get_file_attr(const char *name,  void *value, size_t size); //lhd get filequota  
  int _get_group_file_attr(const char *name,  void *value, size_t size); //lhd get group filequota

  
  int _setxattr_check_quota(Inode* in);//lhd check
  int check_quota_in_inode(Inode* in ,map<uid_t, uint64_t> &uquota_data_map,map<uid_t, uint32_t>& uquota_file_map,
  	                          map<gid_t, uint64_t>& gquota_data_map,map<gid_t, uint32_t>& gquota_file_map);  //lhd check by inode

  /*group quota*/
  int _setxattr_group_quota(const char *name, const void *value, size_t size, int flags);//lhd set groupquota
  int _set_group_quota_attr(const char *name, const void *value, size_t size, string op); //lhd set groupquota
  int _del_group_quota_attr(const char *name, const void *value, size_t size, string op); //lhd delete groupquota   
  int _get_group_quota_attr(const char *name,  void *value, size_t size); //lhd get groupquota
  
  int _removexattr(Inode *in, const char *nm, int uid=-1, int gid=-1);
  int _open(Inode *in, int flags, mode_t mode, Fh **fhp, int uid=-1, int gid=-1);
  int _create(Inode *in, const char *name, int flags, mode_t mode, Inode **inp, Fh **fhp,
              int stripe_unit, int stripe_count, int object_size, const char *data_pool,
              bool *created = NULL, int uid=-1, int gid=-1);
  loff_t _lseek(Fh *fh, loff_t offset, int whence);
  int _read(Fh *fh, int64_t offset, uint64_t size, bufferlist *bl);
  int _write(Fh *fh, int64_t offset, uint64_t size, const char *buf,bufferlist *lpbl = NULL);
  int _flush(Fh *fh);
  int _fsync(Fh *fh, bool syncdataonly);
  int _sync_fs();
  int _fallocate(Fh *fh, int mode, int64_t offset, int64_t length);

  int _getlk(Fh *fh, struct flock *fl, uint64_t owner);
  int _setlk(Fh *fh, struct flock *fl, uint64_t owner, int sleep, void *fuse_req=NULL);
  int _flock(Fh *fh, int cmd, uint64_t owner, void *fuse_req=NULL);

  int get_or_create(Inode *dir, const char* name,
                    Dentry **pdn, bool expect_null=false);

  enum
  {
      NO_ACL = 0,
      POSIX_ACL = 1
  };

  class RequestUserGroups : public UserGroups
  {
      Client *client;
      uid_t uid;
      gid_t gid;
      int sgid_count;
      gid_t *sgids;
      void init()
      {
          sgid_count = client->_getgrouplist(&sgids, uid, gid);
      }

  public:
      RequestUserGroups(Client *c, uid_t u, gid_t g) :
        client(c), uid(u), gid(g), sgid_count(-1), sgids(NULL)
        {}
      ~RequestUserGroups()
      {
          delete sgids;
      }

      gid_t get_gid() { return gid; }
      bool is_in(gid_t id);
      int get_gids(const gid_t **out);
  };

  int check_permissions(Inode *in, int flags, int uid, int gid);

  int inode_permission(Inode *in, uid_t uid, UserGroups &groups, unsigned want);
  int xattr_permission(Inode *in, const char *name, unsigned want, int uid = -1, int gid = -1);
  int may_setattr(Inode *in, struct stat *st, int mask, int uid = -1, int gid = -1);
  int may_open(Inode *in, int flags, int uid = -1, int gid = -1);
  int may_lookup(Inode *dir, int uid = -1, int gid = -1);
  int may_create(Inode *dir, int uid = -1, int gid = -1);
  int may_delete(Inode *dir, const char *name, int uid = -1, int gid = -1);
  int may_hardlink(Inode *in, int uid = -1, int gid = -1);
  int _getattr_for_perm(Inode *in, int uid, int gid);
  int _getgrouplist(gid_t **sgids, int uid, int gid);

  vinodeno_t _get_vino(Inode *in);
  inodeno_t _get_inodeno(Inode *in);

  /*
   * These define virtual xattrs exposing the recursive directory
   * statistics and layout metadata.
   */
  struct VXattr {
    const string name;
    size_t (Client::*getxattr_cb)(Inode *in, char *val, size_t size);
    bool readonly, hidden;
    bool (Client::*exists_cb)(Inode *in);
  };
  bool _vxattrcb_quota_exists(Inode *in);
  size_t _vxattrcb_quota(Inode *in, char *val, size_t size);
  size_t _vxattrcb_quota_max_Gbytes(Inode *in, char *val, size_t size);
  size_t _vxattrcb_quota_max_files(Inode *in, char *val, size_t size);
  
  bool _vxattrcb_layout_exists(Inode *in);
  size_t _vxattrcb_layout(Inode *in, char *val, size_t size);
  size_t _vxattrcb_layout_stripe_unit(Inode *in, char *val, size_t size);
  size_t _vxattrcb_layout_stripe_count(Inode *in, char *val, size_t size);
  size_t _vxattrcb_layout_object_size(Inode *in, char *val, size_t size);
  size_t _vxattrcb_layout_losf_write(Inode *in, char *val, size_t size);
  size_t _vxattrcb_layout_pool(Inode *in, char *val, size_t size);
  size_t _vxattrcb_dir_entries(Inode *in, char *val, size_t size);
  size_t _vxattrcb_dir_files(Inode *in, char *val, size_t size);
  size_t _vxattrcb_dir_subdirs(Inode *in, char *val, size_t size);
  size_t _vxattrcb_dir_rentries(Inode *in, char *val, size_t size);
  size_t _vxattrcb_dir_rfiles(Inode *in, char *val, size_t size);
  size_t _vxattrcb_dir_rsubdirs(Inode *in, char *val, size_t size);
  size_t _vxattrcb_dir_rbytes(Inode *in, char *val, size_t size);
  size_t _vxattrcb_dir_rctime(Inode *in, char *val, size_t size);
  size_t _vxattrs_calcu_name_size(const VXattr *vxattrs);

  static const VXattr _dir_vxattrs[];
  static const VXattr _file_vxattrs[];

  static const VXattr *_get_vxattrs(Inode *in);
  static const VXattr *_match_vxattr(Inode *in, const char *name);

  size_t _file_vxattrs_name_size;
  size_t _dir_vxattrs_name_size;
  size_t _vxattrs_name_size(const VXattr *vxattrs)
  {
    if (vxattrs == _dir_vxattrs)
      return _dir_vxattrs_name_size;
    else if (vxattrs == _file_vxattrs)
      return _file_vxattrs_name_size;
    return 0;
  }

  int _do_filelock(Inode *in, Fh *fh, int lock_type, int op, int sleep,
		   struct flock *fl, uint64_t owner, void *fuse_req=NULL);
  int _interrupt_filelock(MetaRequest *req);
  void _encode_filelocks(Inode *in, bufferlist& bl);
  void _release_filelocks(Fh *fh);
  void _update_lock_state(struct flock *fl, uint64_t owner, ceph_lock_state_t *lock_state);

  int _posix_acl_permission(Inode *in, uid_t uid, UserGroups &groups, unsigned want);

public:
  int mount(const std::string &mount_root);
  void unmount();

  // these shoud (more or less) mirror the actual system calls.
  int statfs(const char *path, struct statvfs *stbuf, Inode *in = 0);

  // crap
  int chdir(const char *s);
  void getcwd(std::string& cwd);

  // namespace ops
  int opendir(const char *name, dir_result_t **dirpp);
  int closedir(dir_result_t *dirp);

  /**
   * Fill a directory listing from dirp, invoking cb for each entry
   * with the given pointer, the dirent, the struct stat, the stmask,
   * and the offset.
   *
   * Returns 0 if it reached the end of the directory.
   * If @a cb returns a negative error code, stop and return that.
   */
  int readdir_r_cb(dir_result_t *dirp, add_dirent_cb_t cb, void *p);

  struct dirent * readdir(dir_result_t *d);
  int readdir_r(dir_result_t *dirp, struct dirent *de);
  int readdirplus_r(dir_result_t *dirp, struct dirent *de, struct stat *st, int *stmask);

  int getdir(const char *relpath, list<string>& names);  // get the whole dir at once.

  /**
   * Returns the length of the buffer that got filled in, or -errno.
   * If it returns -ERANGE you just need to increase the size of the
   * buffer and try again.
   */
  int _getdents(dir_result_t *dirp, char *buf, int buflen, bool ful);  // get a bunch of dentries at once
  int getdents(dir_result_t *dirp, char *buf, int buflen)
  {
    return _getdents(dirp, buf, buflen, true);
  }
  int getdnames(dir_result_t *dirp, char *buf, int buflen)
  {
    return _getdents(dirp, buf, buflen, false);
  }

  void rewinddir(dir_result_t *dirp);
  loff_t telldir(dir_result_t *dirp);
  void seekdir(dir_result_t *dirp, loff_t offset);

  int link(const char *existing, const char *newname);
  int unlink(const char *path);
  int rename(const char *from, const char *to);

  // dirs
  int mkdir(const char *path, mode_t mode);
  int mkdirs(const char *path, mode_t mode);
  int rmdir(const char *path);

  // symlinks
  int readlink(const char *path, char *buf, loff_t size);

  int symlink(const char *existing, const char *newname);

  // inode stuff
  int stat(const char *path, struct stat *stbuf, frag_info_t *dirstat=0, int mask=CEPH_STAT_CAP_INODE_ALL);
  int lstat(const char *path, struct stat *stbuf, frag_info_t *dirstat=0, int mask=CEPH_STAT_CAP_INODE_ALL);
  int lstatlite(const char *path, struct statlite *buf);

  int setattr(const char *relpath, struct stat *attr, int mask);
  int fsetattr(int fd, struct stat *attr, int mask);
  int chmod(const char *path, mode_t mode);
  int fchmod(int fd, mode_t mode);
  int lchmod(const char *path, mode_t mode);
  int chown(const char *path, int uid, int gid);
  int fchown(int fd, int uid, int gid);
  int lchown(const char *path, int uid, int gid);
  int utime(const char *path, struct utimbuf *buf);
  int lutime(const char *path, struct utimbuf *buf);
  int truncate(const char *path, loff_t size);

  // file ops
  int mknod(const char *path, mode_t mode, dev_t rdev=0);
  int open(const char *path, int flags, mode_t mode=0);
  int open(const char *path, int flags, mode_t mode, int stripe_unit, int stripe_count, int object_size, const char *data_pool);
  int set_work_id(int fd, int work_id);
  int get_work_id(int fd, int *work_id);
  int lookup_hash(inodeno_t ino, inodeno_t dirino, const char *name);
  int lookup_ino(inodeno_t ino, Inode **inode=NULL);
  int lookup_parent(Inode *in, Inode **parent=NULL);
  int lookup_name(Inode *in, Inode *parent);
  int close(int fd);
  loff_t lseek(int fd, loff_t offset, int whence);
  int read(int fd, char *buf, loff_t size, loff_t offset=-1);
  int write(int fd, const char *buf, loff_t size, loff_t offset=-1);
  int fake_write_size(int fd, loff_t size);
  int ftruncate(int fd, loff_t size);
  int fsync(int fd, bool syncdataonly);
  int fstat(int fd, struct stat *stbuf);
  int fallocate(int fd, int mode, loff_t offset, loff_t length);

  // full path xattr ops
  int getxattr(const char *path, const char *name, void *value, size_t size);
  int lgetxattr(const char *path, const char *name, void *value, size_t size);
  int listxattr(const char *path, char *list, size_t size);
  int llistxattr(const char *path, char *list, size_t size);
  int removexattr(const char *path, const char *name);
  int lremovexattr(const char *path, const char *name);
  int setxattr(const char *path, const char *name, const void *value, size_t size, int flags);
  int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags);

  int sync_fs();
  int64_t drop_caches();

  // hpc lazyio
  int lazyio_propogate(int fd, loff_t offset, size_t count);
  int lazyio_synchronize(int fd, loff_t offset, size_t count);

  // expose file layout
  int describe_layout(const char *path, ceph_file_layout* layout);
  int fdescribe_layout(int fd, ceph_file_layout* layout);
  int get_file_stripe_address(int fd, loff_t offset, vector<entity_addr_t>& address);
  int get_file_extent_osds(int fd, loff_t off, loff_t *len, vector<int>& osds);
  int get_osd_addr(int osd, entity_addr_t& addr);

  // expose osdmap
  int get_local_osd();
  int get_pool_replication(int64_t pool);
  int64_t get_pool_id(const char *pool_name);
  string get_pool_name(int64_t pool);
  int get_osd_crush_location(int id, vector<pair<string, string> >& path);

  int enumerate_layout(int fd, vector<ObjectExtent>& result,
                       loff_t length, loff_t offset);

  int mksnap(const char *path, const char *name);
  int rmsnap(const char *path, const char *name);

  // expose caps
  int get_caps_issued(int fd);
  int get_caps_issued(const char *path);

  // low-level interface v2
  inodeno_t ll_get_inodeno(Inode *in)
  {
    Mutex::Locker lock(client_lock);
    return _get_inodeno(in);
  }
  snapid_t ll_get_snapid(Inode *in);
  vinodeno_t ll_get_vino(Inode *in)
  {
    Mutex::Locker lock(client_lock);
    return _get_vino(in);
  }
  Inode *ll_get_inode(vinodeno_t vino);
  int ll_lookup(Inode *parent, const char *name, struct stat *attr,
                Inode **out, int uid = -1, int gid = -1);
  bool ll_forget(Inode *in, int count);
  bool ll_put(Inode *in);
  int ll_getattr(Inode *in, struct stat *st, int uid = -1, int gid = -1);
  int ll_setattr(Inode *in, struct stat *st, int mask, int uid = -1,
                 int gid = -1);
  int ll_getxattr(Inode *in, const char *name, void *value, size_t size,
                  int uid=-1, int gid=-1);
  int ll_setxattr(Inode *in, const char *name, const void *value, size_t size,
                  int flags, int uid=-1, int gid=-1);
  int ll_removexattr(Inode *in, const char *name, int uid=-1, int gid=-1);
  int ll_listxattr(Inode *in, char *list, size_t size, int uid=-1, int gid=-1);
  int ll_opendir(Inode *in, dir_result_t **dirpp, int uid = -1, int gid = -1);
  int ll_releasedir(dir_result_t* dirp);
  int ll_readlink(Inode *in, char *buf, size_t bufsize, int uid = -1, int gid = -1);
  int ll_mknod(Inode *in, const char *name, mode_t mode, dev_t rdev,
               struct stat *attr, Inode **out, int uid = -1, int gid = -1);
  int ll_mkdir(Inode *in, const char *name, mode_t mode, struct stat *attr,
               Inode **out, int uid = -1, int gid = -1);
  int ll_symlink(Inode *in, const char *name, const char *value,
                 struct stat *attr, Inode **out, int uid = -1, int gid = -1);
  int ll_unlink(Inode *in, const char *name, int uid = -1, int gid = -1);
  int ll_rmdir(Inode *in, const char *name, int uid = -1, int gid = -1);
  int ll_rename(Inode *parent, const char *name, Inode *newparent,
                const char *newname, int uid = -1, int gid = -1);
  int ll_link(Inode *in, Inode *newparent, const char *newname,
              struct stat *attr, int uid = -1, int gid = -1);
  int ll_open(Inode *in, int flags, Fh **fh, int uid = -1, int gid = -1);
  int ll_create(Inode *parent, const char *name, mode_t mode, int flags,
                struct stat *attr, Inode **out, Fh **fhp, int uid = -1,
                int gid = -1);
  int ll_read_block(Inode *in, uint64_t blockid, char *buf,  uint64_t offset,
                    uint64_t length, ceph_file_layout* layout);

  int ll_write_block(Inode *in, uint64_t blockid,
                     char* buf, uint64_t offset,
                     uint64_t length, ceph_file_layout* layout,
                     uint64_t snapseq, uint32_t sync);
  int ll_commit_blocks(Inode *in, uint64_t offset, uint64_t length);

  int ll_statfs(Inode *in, struct statvfs *stbuf);
  int ll_walk(const char* name, Inode **i, struct stat *attr); // XXX in?
  int ll_listxattr_chunks(Inode *in, char *names, size_t size,
                          int *cookie, int *eol, int uid, int gid);
  uint32_t ll_stripe_unit(Inode *in);
  int ll_file_layout(Inode *in, ceph_file_layout *layout);
  uint64_t ll_snap_seq(Inode *in);

  int ll_read(Fh *fh, loff_t off, loff_t len, bufferlist *bl);
  int ll_write(Fh *fh, loff_t off, loff_t len, const char *data);
  loff_t ll_lseek(Fh *fh, loff_t offset, int whence);
  int ll_flush(Fh *fh);
  int ll_fsync(Fh *fh, bool syncdataonly);
  int ll_fallocate(Fh *fh, int mode, loff_t offset, loff_t length);
  int ll_release(Fh *fh);

  int ll_getlk(Fh *fh, struct flock *fl, uint64_t owner);
  int ll_setlk(Fh *fh, struct flock *fl, uint64_t owner, int sleep, void *fuse_req);
  int ll_flock(Fh *fh, int cmd, uint64_t owner, void *fuse_req);
  void ll_interrupt(void *d);

  int ll_get_stripe_osd(struct Inode *in, uint64_t blockno,
                        ceph_file_layout* layout);
  uint64_t ll_get_internal_offset(struct Inode *in, uint64_t blockno);

  int ll_num_osds(void);
  int ll_osdaddr(int osd, uint32_t *addr);
  int ll_osdaddr(int osd, char* buf, size_t size);

  void ll_register_callbacks(struct client_callback_args *args);
  void ll_register_switch_interrupt_cb(client_switch_interrupt_callback_t cb);
 
  void dump_calc_mds( Formatter *f, const char * relpath ); 

//add by li.jiebj
  int user_auth(string username, string passwd, int *uid, int *gid);//zzg
  int ll_chmod(Inode *in, mode_t mode);
  int ll_chown(Inode *in, int uid, int gid);
  int ll_stat(Inode *in, struct stat *stbuf,frag_info_t *dirstat =0, int mask=CEPH_STAT_CAP_INODE_ALL);

  int ll_permission_walk(Inode *in, uid_t uid, gid_t gid, int perm_chk)
  {
    return pacl->permission_walk( in, uid,gid, perm_chk);
  }
  int ll_permission_walk_parent(Inode *in, uid_t uid, gid_t gid, int perm_chk)
  {
    return pacl->permission_walk_parent( in, uid, gid, perm_chk);
  }
  int ll_init_acl(Inode *in, umode_t i_mode)
  {
    return pacl->fuse_init_acl(in,i_mode);
  }
  int ll_disable_acl_mask(Inode *in)
  {
    return pacl->fuse_disable_acl_mask(in);
  }
  int ll_inherit_acl(Inode *in)
  {
    return pacl->fuse_inherit_acl(in);
  }
  int ll_acl_mask_to_groupmode(Inode *in,int uid,int gid)
  {
    return pacl->fuse_acl_mask_to_groupmode(in,uid,gid);
  }
  int ll_acl_to_ugo(Inode *in, const char *acl_xattr, int length,int uid,int gid)
  {
    return pacl->fuse_acl_to_ugo(in,acl_xattr,length);
  }
  int ll_ugo_to_acl(Inode *in, const int mode)
  {
    return pacl->fuse_ugo_to_acl(in,mode);
  }
  int ll_fuse_init_acl(Inode *in, umode_t i_mode)
  {
    return pacl->fuse_init_acl(in,i_mode);
  }

  //add by lvq
  int ll_check_sticky_parent(struct Inode *in, struct Inode *inChild, uid_t uid)
  {
    if (pacl->check_sticky_parent(in, inChild, uid))
      return -EPERM;

    return 0;
  }

  bool is_parent_acl(struct Inode *in, uid_t uid, gid_t gid)
  {
        return pacl->is_parent_acl(in, uid, gid);
  }
  //add by lvq end
//add by li.jiebj

  map<uid_t, UserQuotaData> user_quota_map;
  map<gid_t, GrpQuotaData> group_quota_map;
  map<uid_t, UserFileQuota> user_file_quota_map; 
  map<gid_t, GrpFileQuota> group_file_quota_map;

  map<inodeno_t, DirQuotaData> dir_quota_map;
  int64_t dir_quota_report; 
  uint64_t sysMaxB;
  uint64_t sysMaxGB;
  int user_quota_init(void); 
  void handle_uquota_update(map<uid_t,UserQuotaData> &quota_map);
  void uquota_proc_chown(Inode *in, uid_t old_uid,gid_t old_gid, struct stat *attr, int mask, uint64_t old_size);
  int uquota_ad_proc_write(Inode *in, uint64_t endoff, UserQuotaData **uquota_data ,GrpQuotaData **gquota_data);
  void uquota_proc_open(Inode *in,int64_t offsize);
  void uquota_proc_unlink(Inode *in);
  void uquota_proc_write(Inode *in, int64_t new_bytes, UserQuotaData *uquota_data ,GrpQuotaData *gquota_data);
  void uquota_pro_fallocate(Inode *in, int64_t new_bytes);
  void uquota_pro_symlink(Inode *in,int64_t new_bytes);
  void uquota_ad_rmdir(Inode *in, map<uid_t, UserQuotaData> &quota_map_tmp,map<gid_t, GrpQuotaData> &group_map_tmp,
                          map<uid_t, UserFileQuota> &file_map_tmp,map<uid_t, GrpFileQuota> &grp_file_map_tmp);

  void uquota_pro_rmdir( map<uid_t, UserQuotaData> &quota_map_tmp,map<gid_t, GrpQuotaData> &group_map_tmp,
                          map<uid_t, UserFileQuota> &file_map_tmp,map<uid_t, GrpFileQuota> &grp_file_map_tmp);
  void ceph_user_quota_inc_used_report(map<uid_t,UserQuotaData> &quota_map, bool rate);
  bool ceph_user_quota_bytes_exceeded(int64_t new_bytes, UserQuotaData *user_quota_data);
  bool ceph_user_quota_bytes_approaching(UserQuotaData *user_quota_data);
  bool is_user_quota_bytes_report(const UserQuotaData *uq_data);
  bool user_quota_changed(uid_t uid, int64_t newbytes,UserQuotaData **uq_data);
  //user quota
  int get_user_quota(uid_t id,UserQuotaData&qt);
  int ceph_get_user_quota(uid_t uid,gid_t gid,uint64_t *hard, uint64_t *soft, uint64_t *used);
  int set_user_quota(const UserQuotaData &qt);
  int modify_user_quota(const UserQuotaData &qt);
  int delete_user_quota(const UserQuotaData &qt);
  int clear_user_quota(void);
  //file quota
  int file_quota_init(void);
  uint32_t get_user_file(uid_t id);
  bool file_quota_changed(uid_t id ,int32_t vary);  
  void file_quota_report(map<uid_t,UserFileQuota> &file_quota_map,bool rate);
  //group file quota  
  uint32_t get_group_file(gid_t id);
  bool group_file_quota_changed(gid_t id ,int32_t vary);  
  void  group_file_quota_report(map<gid_t,GrpFileQuota> &file_quota_map,bool rate);
  //group quota 
  int get_group_quota(gid_t id,GrpQuotaData&qt);  
  int set_group_quota(const GrpQuotaData &qt);
  int modify_group_quota(const GrpQuotaData &qt);
  int delete_group_quota(const GrpQuotaData &qt);
  int clear_group_quota(void);
  bool group_quota_used_exceeded(int64_t new_bytes,const GrpQuotaData *qt);
  void group_quota_used_approaching(GrpQuotaData *qt);
  bool is_group_quota_used_report(const GrpQuotaData *qt);
  bool group_quota_changed(gid_t gid, int64_t newbytes,GrpQuotaData **qt);
  void group_quota_used_report(map<gid_t,GrpQuotaData> &quota_map, bool rate);  
  void handle_group_quota_update(map<gid_t,GrpQuotaData> &quota_map);
  /*dir quota*/
  int dir_quota_init(void);
  void handle_dir_quota_broadcast(MClientDirQuota *m);
  void handle_dir_quota_set(DirQuotaData &newDirQuota);
  void handle_dir_quota_del(const DirQuotaData &delDirQuota);
  void handle_dir_quota_update(map<inodeno_t, DirQuotaData> &update_quota_map);
  int set_dir_quota(Inode *quota_inode);
  int delete_dir_quota(Inode *quota_inode);
  int get_dir_quota(inodeno_t _ino, DirQuotaData & _dirQuota);
  int ceph_loop_calc_subdir_quota(Inode *in, int64_t *used_Gbytes);
  void ceph_quota_get_child_total(Inode *in, int64_t *used_Gbytes);
  void get_sys_maxbytes(void);
  bool ceph_dir_quota_para_check(Inode *in, int64_t new_val, int uid, int gid);
  int ceph_provision_para_check(Inode *in, int64_t new_val, int uid, int gid);
  bool ceph_quota_bytes_exceeded(Inode *in, uint64_t new_bytes, Inode *quota_inode);
  bool ceph_quota_bytes_approaching(Inode *quota_inode, int64_t new_bytes);
  bool is_dir_quota_bytes_report(const DirQuotaData *dir_quota);
  void ceph_dir_quota_inc_used_report(map<inodeno_t, DirQuotaData> &report_quota_map, bool rate);
  void update_recursion_inode_quota_bytes(Inode *quota_inode, int64_t new_bytes);
  void count_rename_quota(Inode *from_quota, Inode *to_quota, uint64_t size);
  bool dquota_ad_proc_rename(Inode **from_quota, Inode **to_quota,
                                   Inode *fromdir, Inode *todir, 
                                   Inode *oldin, uint64_t *size);
  //added for get disk_quota
  int ceph_get_disk_quota(const char *path, struct statvfs *stbuf);
  
};

#endif
