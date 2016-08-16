#ifndef CEPH_CLIENT_FH_H
#define CEPH_CLIENT_FH_H

#include "include/types.h"

struct Inode;
class Cond;
class ceph_lock_state_t;
// file handle for any open file state

struct Fh {
  Inode    *inode;
  loff_t    pos;
  int       mds;        // have to talk to mds we opened with (for now)
  int       mode;       // the mode i opened the file with
  int       work_id;    // ���ļ�ʹ�õ��Ǹ��߳�
  
  int flags;
  bool pos_locked;           // pos is currently in use
  list<Cond*> pos_waiters;   // waiters for pos

  // readahead state
  loff_t last_pos;
  loff_t consec_read_bytes;
  int nr_consec_read;

  ceph_lock_state_t *fcntl_locks;
  ceph_lock_state_t *flock_locks;


  xlist<Fh*>::item delay_close; // losf

  Fh() : inode(0), pos(0), mds(0), mode(0), work_id(-1), flags(0), pos_locked(false),
	 last_pos(0), consec_read_bytes(0), nr_consec_read(0),
         fcntl_locks(NULL), flock_locks(NULL), delay_close(this) {}
};


#endif
