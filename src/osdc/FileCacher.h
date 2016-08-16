// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
#ifndef CEPH_FILECACHER_H
#define CEPH_FILECACHER_H

#include "ObjectCacher.h"
#include "client/Inode.h"
#include <fstream>
#include <algorithm>
#include <functional>
#include <vector>
#include <list>
using namespace std;


class FileCacher:public ObjectCacher {

private:
  uint64_t max_readahead;
  double max_readcache_period;
  pair<loff_t,uint64_t> calc_readahead_pos(map<loff_t,uint64_t> &fcs,loff_t off,uint64_t max_can_rd,uint64_t filesize,ceph_file_layout *layout);//add by li.jiebj
  loff_t calc_readahead_off(map<loff_t,uint64_t> &fcs,loff_t off);//add by li.jiebj
  newObjectManager<Object> free_object_list;
  newObjectManager<struct OSDRead> free_osdread_list;
  newObjectManager<BufferHead> free_bh_list;
  newObjectManager<struct OSDWrite> free_osdwrite_list;
public:
	typedef void (*update_cap_ref_t) (void *p, Inode *in,int cap);//cap>0 get ; cap<0 put
  FileCacher(CephContext *cct_, string name, WritebackHandler& wb, Mutex& l,
	       flush_set_callback_t flush_callback,
	       void *flush_callback_arg,
	       update_cap_ref_t _update_cap_ref,
	       void* _update_cap_ref_arg,
	       uint64_t max_bytes, uint64_t max_objects,
	       uint64_t max_dirty, uint64_t target_dirty, double max_age,
	       bool block_writes_upfront,uint64_t read_total_bytes_per,double readcache_period);
  ~FileCacher();
  update_cap_ref_t update_cap_ref;
  void* update_cap_ref_arg;
  virtual void start() {
    ObjectCacher::start();
    //lpcur_list_delete_bh = &(list_delete_bh[0]);
    lpcur_list_delete_ptr = &(_list_delete_ptr[0]);
    idelete_bh_index = 0;
    delete_bh_thread.create();
    asend_op_thread.create();   
    //asend_wop_thread.create();  
  }
  
  virtual void stop() {
    
    assert(delete_bh_thread.is_started());
    lock.Lock();
    delete_bh_stoped = true;
    lock.Unlock();
    delete_bh_thread.join();
 
    assert(asend_op_thread.is_started());
    async_send_lock.Lock();
    is_asend_stopped = true;
    async_send_lock.Unlock();
    asend_op_thread.join();

    ObjectCacher::stop();
  }
  
  Object *get_object(sobject_t oid, ObjectSet *oset, object_locator_t &l,
		     uint64_t truncate_size,uint64_t fileoff, uint64_t truncate_seq);
  
  Object *get_object(uint64_t ino,uint64_t blockno, ObjectSet *oset, object_locator_t &l,
		     uint64_t truncate_size,uint64_t fileoff, uint64_t truncate_seq);

  int file_writex(ObjectSet *oset, ceph_file_layout *layout, const SnapContext& snapc,
                  loff_t offset, uint64_t len,
                  bufferlist& bl,utime_t mtime, int flags,
                  Mutex & wait_on_lock);
  int writex(OSDWrite *wr, ObjectSet *oset, Mutex& wait_on_lock,
             Context *onfreespace);
  void close_object(Object *ob);
  virtual bool fcache_in_scop(uint64_t rx_size){return rx_size < max_readahead_size;}
  void update_fcsection(BufferHead *bh,int mode);
  virtual int file_write(ObjectSet *oset, ceph_file_layout *layout, const SnapContext& snapc,
                 loff_t offset, uint64_t len, 
                 bufferlist& bl, utime_t mtime, int flags,
		             Mutex& wait_on_lock);
  virtual int file_read(ObjectSet *oset, ceph_file_layout *layout, snapid_t snapid,
                loff_t offset, uint64_t len, 
                bufferlist *bl,int flags,
                Context *onfinish,uint64_t oldlen=0);
  virtual void bh_read_finish(int64_t poolid, Object *pOb, ceph_tid_t tid,
		      loff_t offset, uint64_t length,
		      bufferlist &bl, int r,
		      bool trust_enoent);
  virtual void bh_write_commit(int64_t poolid, Object *pOb, loff_t offset,
		       uint64_t length, ceph_tid_t t, int r);
  virtual void trim(ObjectSet *oset);
  virtual uint64_t release_all();
  virtual void discard_set(ObjectSet *oset, vector<ObjectExtent>& exls);
  virtual bool flush_set(ObjectSet *oset, vector<ObjectExtent>& exv, Context *onfinish);
  virtual bool flush_set(ObjectSet *oset, Context *onfinish);
  virtual int file_is_cached(ObjectSet *oset, ceph_file_layout *layout, snapid_t snapid,
		     loff_t offset, uint64_t len){return 0;}
  virtual int _readx(OSDRead *rd, ObjectSet *oset, Context *onfinish,
	     bool external_call);
	virtual void bh_remove(Object *ob, BufferHead *bh);
	virtual void touch_bh(BufferHead *bh) {
		bh->touch_self();
    touch_ob(bh->ob);
  }
  virtual void bh_set_state(BufferHead *bh, int s);//add by lijie
  virtual OSDRead *prepare_read(snapid_t snap, bufferlist *b, int f) {
    OSDRead *tmp = free_osdread_list.allocobj();
    tmp->InitObj(snap,b,f);
    return tmp;
  }
  virtual OSDWrite *prepare_write(const SnapContext& sc, bufferlist &b, utime_t mt, int f) {
    OSDWrite *tmp = free_osdwrite_list.allocobj();
    tmp->InitObj(sc,b,mt,f);
    return tmp;
  }
  virtual void bh_write(BufferHead *bh);
  BufferHead *allocbh(Object *ob);
  void releasebh(BufferHead *bh);
private:
  uint64_t file_read_count;
  uint64_t file_write_count;
  uint64_t file_cache_quota;

  LRU lru_filereadtime;
  LRU lru_filewritetime;
  uint64_t max_readahead_size;
  struct DirtyOSetDirtyCompare{
    bool operator()(ObjectSet *osetl, ObjectSet *osetr) const 
    {
        return osetl->dirty_size >osetr->dirty_size;
    }
  };
  struct DirtyBHDirtyCompare{
    bool operator()(BufferHead* bhl,BufferHead*  bhr) const 
    {
        return (bhl->get_state() == ObjectCacher::BufferHead::STATE_DIRTY &&
                bhl->length() > bhr->length());
    }
  };
  list<ObjectSet *> dirty_record_map; // 记载个文件的脏数据量_
  
protected:
	//ofstream ofs;
  virtual void bh_stat_add(BufferHead *bh);
  virtual void bh_stat_sub(BufferHead *bh);
  virtual void touch_ob(Object *ob) {
    ob->oset->ob_lru.lru_touch(ob);
  }
  #define OPREAD 1
  #define OPWRITE 2
  void touch_file(int OpType,ObjectSet *oset);
  virtual void flusher_entry();

  virtual void delete_bh_entry();
  int idelete_bh_index;
  std::list<bufferptr> _list_delete_ptr[2];
  std::list<bufferptr> *lpcur_list_delete_ptr;
  uint64_t delete_bh_count;
  uint64_t delete_bh_index;
  bool delete_bh_stoped;
  class DeleteBhThread : public Thread {
    FileCacher *oc;
  public:
    DeleteBhThread(FileCacher *o) : oc(o) {}
    void *entry() {
      oc->delete_bh_entry();
      return 0;
    }
  } delete_bh_thread;


  virtual void bh_read(BufferHead *bh);
  std::list<BufferHead* > oplist;
  void async_send_op();
  bool is_asend_stopped;
  Mutex async_send_lock;
  class AsyncSendOpThread : public Thread {
    FileCacher *fc;
  public:
    AsyncSendOpThread(FileCacher *_fc) : fc(_fc) {}
    void *entry() {
      fc->async_send_op();
      return 0;
    }
  } asend_op_thread;


  //async send write BH , add by li.jiebj
  std::list<BufferHead* > woplist ;
  
  void async_send_wop();
  Mutex async_wsend_lock; 

};



#endif
