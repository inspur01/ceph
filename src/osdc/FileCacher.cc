// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "FileCacher.h"
#include "common/errno.h"
#include "WritebackHandler.h"


#define dout_subsys ceph_subsys_objectcacher
/*** FileCacher ***/

#undef dout_prefix
#define dout_prefix *_dout << "filecacher "

FileCacher::FileCacher(CephContext *cct_, string name, WritebackHandler& wb, Mutex& l,
                       flush_set_callback_t flush_callback,
                       void *flush_callback_arg,
                       update_cap_ref_t _update_cap_ref,
                       void *_update_cap_ref_arg,
                       uint64_t max_bytes, uint64_t max_objects,
                       uint64_t max_dirty, uint64_t target_dirty, double max_age,
                       bool block_writes_upfront, uint64_t read_total_bytes_per, double readcache_period)
  : ObjectCacher(cct_, name, wb, l, flush_callback, flush_callback_arg, max_bytes, max_objects, max_dirty, target_dirty, max_age, block_writes_upfront),
  	update_cap_ref(_update_cap_ref),update_cap_ref_arg(_update_cap_ref_arg),
    max_readahead(read_total_bytes_per), max_readcache_period(readcache_period), file_read_count(0),
    file_write_count(0),
    max_readahead_size(4194304),//4MB
    file_cache_quota(104857600),
    delete_bh_thread(this),
    delete_bh_stoped(false),
    asend_op_thread(this),
    is_asend_stopped(false),
    async_send_lock("FileCacher::async_send_lock"),
    async_wsend_lock("FileCacher::async_wsend_lock")
   
{
  free_object_list.objcount = 10240;
  free_bh_list.objcount = 10240;
  delete_bh_count = 0;
  delete_bh_index = 0;
}
FileCacher::~FileCacher() {}
ObjectCacher::Object *FileCacher::get_object(sobject_t oid, ObjectSet *oset, object_locator_t &l,
    uint64_t truncate_size, uint64_t fileoff, uint64_t truncate_seq)
{
  // XXX: Add handling of nspace in object_locator_t in cache
  assert(lock.is_locked());
  // have it?
  uint64_t ino;
  uint64_t blockno;
  sscanf(oid.oid.name.c_str(),"%llx.%08llx",&ino,&blockno);
  assert(ino == oset->ino);
  
  return get_object(ino,blockno,oset,l,truncate_size,fileoff,truncate_seq);
}
ObjectCacher::Object *FileCacher::get_object(uint64_t ino,uint64_t blockno, ObjectSet *oset, object_locator_t &l,
		     uint64_t truncate_size,uint64_t fileoff, uint64_t truncate_seq)
{
  // XXX: Add handling of nspace in object_locator_t in cache
  INITCEPHLOG();
  assert(lock.is_locked());
  // have it?
  if (oset->hash_map_blockno_objects.count(blockno)) {
    Object *o = oset->hash_map_blockno_objects[blockno];
    o->truncate_size = truncate_size;
    o->truncate_seq = truncate_seq;
    return o;
  }

  // create it.
  char tmpbuf[128];
  int buflen = sprintf(tmpbuf,"%llx.%08llx",(long long unsigned)oset->ino,blockno);
  sobject_t oid(tmpbuf,CEPH_NOSNAP);
  WCEPHLOG(3511);
  Object *o = free_object_list.allocobj();//new Object(this, oid, oset, l, truncate_size, truncate_seq,fileoff);
  o->InitObj(this,oid,oset,l,truncate_size,truncate_seq,fileoff,blockno);
  WCEPHLOG(3512);
  oset->hash_map_objects[oid] = o;
  oset->hash_map_blockno_objects[blockno] = o;
  oset->ob_lru.lru_insert_top(o);
  return o;

}

void FileCacher::close_object(Object *ob)
{
  INITCEPHLOG();
  assert(lock.is_locked());
  assert(ob->can_close());

  // ok!
  ObjectSet *oset = ob->oset;
  oset->ob_lru.lru_remove(ob);
  WCEPHLOG(35830);
  oset->hash_map_objects.erase(ob->get_soid());
  WCEPHLOG(35831);
  oset->hash_map_blockno_objects.erase(ob->blockno);
  WCEPHLOG(35832);
  //add by wym
  ob->Release();
  WCEPHLOG(35833);
  //delete ob;
  free_object_list.releaseobj(ob);

}


void FileCacher::touch_file(int OpType, ObjectSet *oset)
{
  INITCEPHLOG();
  WCEPHLOG(2100);
  assert(lock.is_locked());
  FileAccessTime *accesstime = NULL;
  WCEPHLOG(2101);
  utime_t now = ceph_clock_now(cct);
  WCEPHLOG(2102);
  utime_t cutoff = now - max_dirty_age;	//default: 5seconds
  FileAccessTime *pFat = NULL;
  int rdcnt = 0, wrcnt = 0;
  if (OpType == OPREAD) {
    accesstime = &(oset->filereadaccess);
    accesstime->time = now;
    lru_filereadtime.lru_remove(accesstime);
    lru_filereadtime.lru_insert_top(accesstime);
  }else if(OpType == OPWRITE){
    accesstime = &(oset->filewriteaccess);
    accesstime->time = now;
    lru_filewritetime.lru_remove(accesstime);
    lru_filewritetime.lru_insert_top(accesstime);
  }else{
    assert(0);
  }
  //process lru_read
  {
    rdcnt = lru_filereadtime.lru_get_size();

    while ((pFat = static_cast<FileAccessTime*>(lru_filereadtime.lru_get_next_expire())) &&
           pFat->time < cutoff )
    {

      if (pFat->oset->dirty_or_tx > 0 || pFat->oset->rx_size > 0) {
        pFat->time = now;
        lru_filereadtime.lru_remove(pFat);
        lru_filereadtime.lru_insert_top(pFat);
        continue;
      }
      BufferHead *bh = NULL;
      while ((bh = static_cast<BufferHead*>(pFat->oset->bh_lru_readahead.lru_pop_next())) != NULL) {
        bh_lru_rest.lru_insert_top(bh);
      }
      assert(pFat->oset->bh_lru_readahead.lru_get_size() == 0);
      lru_filereadtime.lru_remove(pFat);
      rdcnt--;
    }

    file_read_count = rdcnt;

    if (file_read_count)
      max_readahead_size = max_readahead / file_read_count;
  }
  //process lru_write
  {
    wrcnt = lru_filewritetime.lru_get_size();
    if(dirty_record_map.size()<wrcnt)
       oset->reg(&dirty_record_map);
    while ((pFat = static_cast<FileAccessTime*>(lru_filewritetime.lru_get_next_expire())) &&
           pFat->time < cutoff )
    {
      if (pFat->oset->dirty_or_tx > 0 || pFat->oset->rx_size > 0) {
        pFat->time = now;
        lru_filewritetime.lru_remove(pFat);
        lru_filewritetime.lru_insert_top(pFat);
        continue;
      }

      lru_filewritetime.lru_remove(pFat);
      pFat->oset->unreg();
      wrcnt--;
    }

    file_write_count = wrcnt;
  }
  file_cache_quota = cct->_conf->client_oc_size/(file_read_count+file_write_count);
  
}
pair<loff_t, uint64_t> FileCacher::calc_readahead_pos(map<loff_t, uint64_t> &fcs, loff_t off, uint64_t max_can_rd, uint64_t filesize, ceph_file_layout *layout)
{

  if (fcs.size() == 0) //Èç¹ûÎª¿Õ,ÔòÖ±½Ó·µ»Ø×î´ó³¤¶È
    return make_pair(off, max_can_rd);
  loff_t cur = off;
  loff_t left = max_can_rd;

  if (fcs.size() > 1) {
    ldout(cct, 10) << "calc_readahead_pos fcs size:" << fcs.size() << dendl;
  }
  map<loff_t, uint64_t>::iterator it = fcs.lower_bound(cur);
  map<loff_t, uint64_t>::iterator preit = it;
  if (preit != fcs.begin())
    preit--;
  while (it != fcs.end() && left > 0)
  {
    if (preit != it)
      left -=  (it->first - cur <= it->first - preit->first - preit->second) ? it->first - cur : it->first - preit->first - preit->second;
    else //ËµÃ÷it Îªfirst
      left -= it->first - cur;
    if (left > 0)
      cur = it->first + it->second;
    else
      cur = it->first;
    preit = it;
    it++;
  }
  cur += left;


  // Èç¹ûÐ¡ÓÚÎÄ¼þ³¤¶ÈÐèÒª½øÐÐ¶ÔÆë
  if (cur < filesize) {
    loff_t p = layout->fl_stripe_count * layout->fl_object_size;
    if (cur - off >= p)
      // align large readahead with period
      cur -= (cur % p);
    else {
      int su = layout->fl_stripe_unit;
      if (cur - off >= su)
        cur -= (cur % su);
    }
  }
  //²»ÄÜ³¬¹ýÎÄ¼þ³¤¶È
  cur = (cur > filesize ? filesize : cur);


  //¼ÆËã<off,cur> ËùÔÚµÄstartit  £¬ endit  ÒÔ¼°ÐÂµÄ newoff,newcur ,É¾µô startit ºÍ endit Ö®¼äµÄÊý¾Ý£¬È»ºó²åÈëÐÂµÄ newoff,newcur
  //ÆðÊ¼ÓÎ±ê
  map<loff_t, uint64_t>::iterator startit = fcs.lower_bound(off);
  preit = startit;
  loff_t newstart = off;
  if (preit != fcs.begin()) {
    preit--;
    if (preit->first + preit->second >= off) { //Èç¹ûÓëpreÏà½»ÔòÒ²ÐèÒªÉ¾³ý
      startit--;
      newstart = preit->first;
    }
  }


  //ÖÕÖ¹ÓÎ±ê
  map<loff_t, uint64_t>::iterator endit = fcs.upper_bound(cur);
  preit = endit;
  loff_t newend = cur;

  if (preit != fcs.begin()) {
    preit--;
    if (preit->first + preit->second >= cur) {
      newend = preit->first + preit->second;
    }
  }
  fcs.erase(startit, endit);
  fcs.insert(make_pair(newstart, newend - newstart));


  return make_pair(off, cur - off);

}
loff_t FileCacher::calc_readahead_off(map<loff_t, uint64_t> &fcs, loff_t off)
{
  /*
     0. ÔªËØÊýÁ¿Îª¿Õ£¬·µ»Øoff
     1. it == end
     È¡Ç°ÇýÅÐ¶Ï
     2. it == begin
     Ö±½ÓÅÐ¶Ï
     3. ÖÐ¼äÎ»ÖÃ
     È¡Ç°ÇýÅÐ¶Ï
     */
  if (fcs.size() == 0)
    return off;
  map<loff_t, uint64_t>::iterator it = fcs.lower_bound(off);
  map<loff_t, uint64_t>::iterator preit = it;
  map<loff_t, uint64_t>::iterator nextit = it;
  if (it == fcs.end())
  {
    preit--;
    loff_t prend = preit->first + preit->second;
    return ( prend > off) ? prend : off;
  }
  else if (it == fcs.begin()) {
    return (it->first > off) ? off : (it->first + it->second);
  }
  else {
    preit--;
    if (preit->first + preit->second >= off)
      return preit->first + preit->second;
    else if (preit->first + preit->second < off && off < it->first)
      return off;
    else
      return it->first + it->second;
  }
}

int FileCacher::file_write(ObjectSet *oset, ceph_file_layout *layout, const SnapContext& snapc,
                           loff_t offset, uint64_t len,
                           bufferlist& bl, utime_t mtime, int flags,
                           Mutex& wait_on_lock)
{
  //add by wym. µ±Ð´ÈëÊý¾ÝÁ¿³¬¹ýOCÔÊÐíµÄÔàÊý¾ÝÁ¿ÉÏÏÞÊ±£¬Ôò´¥·¢ÔàÊý¾ÝË¢ÐÂ²¢×èÈûÔÚ´Ë´¦
  INITCEPHLOG();
  touch_file(OPWRITE, oset);
  WCEPHLOG(211);
  if (oset->dirty_or_tx != 0 && oset->dirty_or_tx + len >= file_cache_quota)
  {
    lock.Unlock();
    WCEPHLOG(212);
    oset->dirty_wait_mutex.Lock();
    do
    {
      //flusher_cond.Signal();
      oset->is_dirty_max = true;
      oset->dirty_wait_cond.WaitInterval(cct, oset->dirty_wait_mutex, utime_t(1, 0));
    } while (oset->dirty_or_tx != 0 && oset->dirty_or_tx + len >= file_cache_quota);
    oset->dirty_wait_mutex.Unlock();
    WCEPHLOG(213);
    lock.Lock();
    Inode *in =  (Inode *)oset->parent;
    assert(in != NULL);
    if (!oset->dirty_or_tx){
      update_cap_ref(update_cap_ref_arg,in, CEPH_CAP_FILE_CACHE | CEPH_CAP_FILE_BUFFER);
    }
  }

  oset->is_dirty_max = false;
  WCEPHLOG(214);

  if(unlikely(layout->fl_object_size != layout->fl_stripe_unit || layout->fl_stripe_count != 1 || (((offset % layout->fl_object_size) + len) >layout->fl_object_size)))
    return ObjectCacher::file_write(oset,layout,snapc,offset,len,bl,mtime,flags,wait_on_lock);
  return file_writex(oset,layout,snapc,offset,len,bl,mtime,flags,wait_on_lock);
}
int FileCacher::file_writex(ObjectSet *oset, ceph_file_layout *layout, const SnapContext& snapc,
                  loff_t offset, uint64_t len,
                  bufferlist& bl,utime_t mtime, int flags,
                  Mutex & wait_on_lock) {
  INITCEPHLOG();
  char tmpbuf[128];
  uint64_t fl_object_size = layout->fl_object_size;
  uint64_t blockno = offset / fl_object_size;
  uint64_t blockoffset = offset % fl_object_size;
  uint64_t truncate_size = 0;
  uint64_t trunc_objectsetno = oset->truncate_size / layout->fl_object_size;
  utime_t now = ceph_clock_now(cct);
  if(oset->truncate_size == 0 || oset->truncate_size == (uint64_t)-1)
    truncate_size = oset->truncate_size;
  else {
    if(blockno > trunc_objectsetno)
      truncate_size = 0;
    else if(blockno < trunc_objectsetno)
      truncate_size = fl_object_size;
    else
      truncate_size = oset->truncate_size % fl_object_size;
  }
  struct object_locator_t oloc(layout->fl_pg_pool);
  Object *o = get_object(oset->ino,blockno,oset,oloc,truncate_size,blockno*fl_object_size,oset->truncate_seq);
  WCEPHLOG(21445);

  map<loff_t, BufferHead*>::iterator p = o->data_lower_bound(blockoffset);
  if(unlikely(p != o->data.end()))
    return ObjectCacher::file_write(oset,layout,snapc,offset,len,bl,mtime,flags,wait_on_lock);
  map<loff_t, BufferHead*>::iterator prevp = p;
  if(prevp != o->data.begin())
    --prevp;
  BufferHead *bh = NULL;
  if(prevp == o->data.end() || 
        ((prevp->first+prevp->second->length()) < blockoffset) || 
        (!prevp->second->is_dirty())) {
    bh = allocbh(o);
    bh->set_start(blockoffset);
    bh->set_length(len);
    this->bh_add(o,bh);
  }
  else {
    bh = prevp->second;
    bh_stat_sub(bh);
    bh->set_length(bh->length() + len);
    bh_stat_add(bh);
  }
  bh->bl.claim_append(bl);
  mark_dirty(bh);
  touch_bh(bh);
  bh->last_write = now;
  trim(oset);
  return 0;
}


int FileCacher::file_read(ObjectSet *oset, ceph_file_layout *layout, snapid_t snapid,
                          loff_t offset, uint64_t len,
                          bufferlist *bl, int flags,
                          Context *onfinish, uint64_t oldlen) {
  oset->last_pos = offset;
  if (bl == NULL) {
    //oset->rdtm++;

    Inode *in =  (Inode *)oset->parent;
    assert(in != NULL);
    int ret = -1;

    touch_file(OPREAD, oset);

    loff_t newoff = calc_readahead_off(oset->fc_section, offset);
    uint64_t win = file_cache_quota; //max_read_cache_size;

    loff_t l = in->layout.fl_stripe_count * in->layout.fl_object_size * max_readcache_period;


    if (newoff - offset >= win ||              //  1.³¬³ö´°¿Ú
        newoff >= in->size      ||               //  2.³¬³öÎÄ¼þ³¤¶È
        oset->rx_size + MIN(l, in->size - newoff)  > max_readahead_size || // 3.rx_size ¹ý³¤
        ( newoff - offset > 2 * oldlen &&
          cct->_conf->client_readahead_frequency_ratio * oldlen < layout->fl_object_size &&
          (offset % (uint32_t)(layout->fl_object_size * cct->_conf->client_readahead_obj_mult)) >=  cct->_conf->client_readahead_frequency_ratio * oldlen ) // 4.»º´æ³¤¶È>0 ²¢ÇÒ Î´¿ç¿éµÄÊ±ºò
       )
      return ret;


    //
    if (newoff + l > in->size)
      l = in->size - newoff;


    //
    pair<loff_t, uint64_t> pos = calc_readahead_pos(oset->fc_section, newoff, l, in->size, layout);
    uint64_t newlen = pos.second;


    //
    ret =  ObjectCacher::file_read(oset, layout, snapid, newoff, newlen, NULL, 0, onfinish, 0);

    return ret;
  }
  return ObjectCacher::file_read(oset, layout, snapid, offset, len, bl, flags, onfinish, 0);
}
void FileCacher::update_fcsection(BufferHead *bh, int mode)
{
  loff_t off = bh->start() + bh->ob->foff;
  loff_t cur = bh->end() + bh->ob->foff;
  map<loff_t, uint64_t> &fcs = bh->ob->oset->fc_section;
  map<loff_t, uint64_t>::iterator preit;
  switch (mode)
  {
  case -1: // É¾³ý
  {
    //ÆðÊ¼ÓÎ±ê
    map<loff_t, uint64_t> chg;
    map<loff_t, uint64_t>::iterator startit = fcs.lower_bound(off);
    preit = startit;
    if (preit != fcs.begin()) {
      preit--;
      if (preit->first + preit->second >= off) { //¿ÉÄÜÉú³ÉÐÂµÄ±ä¸üÌõÄ¿
        startit--;
        chg.insert(make_pair(preit->first, off - preit->first));
      }
    }
    //ÖÕÖ¹ÓÎ±ê
    map<loff_t, uint64_t>::iterator endit = fcs.upper_bound(cur);
    preit = endit;
    if (preit != fcs.begin()) {
      preit--;
      if (preit->first + preit->second > cur) {
        chg.insert(make_pair(cur, preit->first + preit->second - cur));
      }
    }
    fcs.erase(startit, endit);
    fcs.insert(chg.begin(), chg.end());

  }
  break;
  case 1:  // Ìí¼Ó
  {
    //ÆðÊ¼ÓÎ±ê
    map<loff_t, uint64_t>::iterator startit = fcs.lower_bound(off);
    preit = startit;
    loff_t newstart = off;
    if (preit != fcs.begin()) {
      preit--;
      if (preit->first + preit->second >= off) { //Èç¹ûÓëpreÏà½»ÔòÒ²ÐèÒªÉ¾³ý
        startit--;
        newstart = preit->first;
      }
    }


    //ÖÕÖ¹ÓÎ±ê
    map<loff_t, uint64_t>::iterator endit = fcs.upper_bound(cur);
    preit = endit;
    loff_t newend = cur;

    if (preit != fcs.begin()) {
      preit--;
      if (preit->first + preit->second >= cur) {
        newend = preit->first + preit->second;
      }
    }
    fcs.erase(startit, endit);
    fcs.insert(make_pair(newstart, newend - newstart));
  }
  break;
  }
}

void FileCacher::bh_stat_add(BufferHead *bh)
{
  assert(lock.is_locked());
  switch (bh->get_state()) {
  case BufferHead::STATE_MISSING: // ¶ÁÐ´ +
    stat_missing += bh->length();
    update_fcsection(bh, 1);     // add by li.jiebj
    break;
  case BufferHead::STATE_CLEAN:
    stat_clean += bh->length(); // merge + Æ½ºâ merge -
    update_fcsection(bh, 1);    // add by li.jiebj
    break;
  case BufferHead::STATE_ZERO:
    stat_zero += bh->length();
    update_fcsection(bh, 1);    // add by li.jiebj
    break;
  case BufferHead::STATE_DIRTY:
    stat_dirty += bh->length();
    bh->ob->dirty_or_tx += bh->length();
    bh->ob->oset->dirty_or_tx += bh->length();
    bh->ob->oset->dirty_size += bh->length();
    break;
  case BufferHead::STATE_TX:
    stat_tx += bh->length();
    bh->ob->dirty_or_tx += bh->length();
    bh->ob->oset->dirty_or_tx += bh->length();
    break;
  case BufferHead::STATE_RX:
    stat_rx += bh->length();
    bh->ob->oset->rx_size += bh->length();//add by li.jiebj
    break;
  case BufferHead::STATE_ERROR:
    stat_error += bh->length();
    break;
  default:
    assert(0 == "bh_stat_add: invalid bufferhead state");
  }
  if (get_stat_dirty_waiting() > 0)
    stat_cond.Signal();
}

void FileCacher::bh_stat_sub(BufferHead *bh)
{
  assert(lock.is_locked());
  switch (bh->get_state()) {
  case BufferHead::STATE_MISSING:
    stat_missing -= bh->length();
    break;
  case BufferHead::STATE_CLEAN: // ÀÏ»¯ - »òÕß merge -
    stat_clean -= bh->length();
    update_fcsection(bh, -1);  // add by li.jiebj
    break;
  case BufferHead::STATE_ZERO:
    stat_zero -= bh->length();
    update_fcsection(bh, -1);  // add by li.jiebj
    break;
  case BufferHead::STATE_DIRTY:
    stat_dirty -= bh->length();
    bh->ob->dirty_or_tx -= bh->length();
    bh->ob->oset->dirty_or_tx -= bh->length();
    bh->ob->oset->dirty_size -= bh->length();
    break;
  case BufferHead::STATE_TX:
    stat_tx -= bh->length();
    bh->ob->dirty_or_tx -= bh->length();
    bh->ob->oset->dirty_or_tx -= bh->length();
    break;
  case BufferHead::STATE_RX:
    stat_rx -= bh->length();
    bh->ob->oset->rx_size -= bh->length();//add by li.jiebj
    break;
  case BufferHead::STATE_ERROR:
    stat_error -= bh->length();
    break;
  default:
    assert(0 == "bh_stat_sub: invalid bufferhead state");
  }
}

void FileCacher::bh_read_finish(int64_t poolid, Object *pOb, ceph_tid_t tid,
                                loff_t start, uint64_t length,
                                bufferlist &bl, int r,
                                bool trust_enoent)
{
  assert(lock.is_locked());
  sobject_t oid = pOb->get_soid();
  ldout(cct, 7) << "bh_read_finish "
                << oid
                << " tid " << tid
                << " " << start << "~" << length
                << " (bl is " << bl.length() << ")"
                << " returned " << r
                << " outstanding reads " << reads_outstanding
                << dendl;

  if (bl.length() < length) {
    bufferptr bp(length - bl.length());
    bp.zero();
    ldout(cct, 7) << "bh_read_finish " << oid << " padding " << start << "~" << length
                  << " with " << bp.length() << " bytes of zeroes" << dendl;
    bl.push_back(bp);
  }

  list<Context*> ls;
  int err = 0;

  if (pOb->oset->hash_map_objects.count(oid) == 0) {//changed by li.jiebj
    ldout(cct, 7) << "bh_read_finish no object cache" << dendl;
  } else {
    Object *ob = pOb;

    if (r == -ENOENT && !ob->complete) {
      // wake up *all* rx waiters, or else we risk reordering identical reads. e.g.
      //   read 1~1
      //   reply to unrelated 3~1 -> !exists
      //   read 1~1 -> immediate ENOENT
      //   reply to first 1~1 -> ooo ENOENT
      bool allzero = true;
      for (map<loff_t, BufferHead*>::iterator p = ob->data.begin(); p != ob->data.end(); ++p) {
        BufferHead *bh = p->second;
        for (map<loff_t, list<Context*> >::iterator p = bh->waitfor_read.begin();
             p != bh->waitfor_read.end();
             ++p)
          ls.splice(ls.end(), p->second);
        bh->waitfor_read.clear();
        if (!bh->is_zero() && !bh->is_rx())
          allzero = false;
      }

      // just pass through and retry all waiters if we don't trust
      // -ENOENT for this read
      if (trust_enoent) {
        ldout(cct, 7) << "bh_read_finish ENOENT, marking complete and !exists on " << *ob << dendl;
        ob->complete = true;
        ob->exists = false;

        /* If all the bhs are effectively zero, get rid of them.  All
         * the waiters will be retried and get -ENOENT immediately, so
         * it's safe to clean up the unneeded bh's now. Since we know
         * it's safe to remove them now, do so, so they aren't hanging
         *around waiting for more -ENOENTs from rados while the cache
         * is being shut down.
         *
         * Only do this when all the bhs are rx or clean, to match the
         * condition in _readx(). If there are any non-rx or non-clean
         * bhs, _readx() will wait for the final result instead of
         * returning -ENOENT immediately.
         */
        if (allzero) {
          ldout(cct, 10) << "bh_read_finish ENOENT and allzero, getting rid of "
                         << "bhs for " << *ob << dendl;
          map<loff_t, BufferHead*>::iterator p = ob->data.begin();
          while (p != ob->data.end()) {
            BufferHead *bh = p->second;
            // current iterator will be invalidated by bh_remove()
            ++p;
            bh_remove(ob, bh);
            //delete bh;
            releasebh(bh);
          }
        }
      }
    }

    // apply to bh's!
    loff_t opos = start;
    while (true) {
      map<loff_t, BufferHead*>::iterator p = ob->data_lower_bound(opos);
      if (p == ob->data.end())
        break;
      if (opos >= start + (loff_t)length) {
        ldout(cct, 20) << "break due to opos " << opos << " >= start+length "
                       << start << "+" << length << "=" << start + (loff_t)length
                       << dendl;
        break;
      }

      BufferHead *bh = p->second;
      ldout(cct, 20) << "checking bh " << *bh << dendl;

      // finishers?
      for (map<loff_t, list<Context*> >::iterator it = bh->waitfor_read.begin();
           it != bh->waitfor_read.end();
           ++it)
        ls.splice(ls.end(), it->second);
      bh->waitfor_read.clear();

      if (bh->start() > opos) {
        ldout(cct, 1) << "bh_read_finish skipping gap "
                      << opos << "~" << bh->start() - opos
                      << dendl;
        opos = bh->start();
        continue;
      }

      if (!bh->is_rx()) {
        ldout(cct, 10) << "bh_read_finish skipping non-rx " << *bh << dendl;
        opos = bh->end();
        continue;
      }

      if (bh->last_read_tid != tid) {
        ldout(cct, 10) << "bh_read_finish bh->last_read_tid " << bh->last_read_tid
                       << " != tid " << tid << ", skipping" << dendl;
        opos = bh->end();
        continue;
      }

      assert(opos >= bh->start());
      assert(bh->start() == opos);   // we don't merge rx bh's... yet!
      assert(bh->length() <= start + (loff_t)length - opos);

      if (bh->error < 0)
        err = bh->error;

      loff_t oldpos = opos;
      opos = bh->end();

      if (r == -ENOENT) {
        if (trust_enoent) {
          ldout(cct, 10) << "bh_read_finish removing " << *bh << dendl;
          bh_remove(ob, bh);
          //delete bh;
          releasebh(bh);
        } else {
          ldout(cct, 10) << "skipping unstrusted -ENOENT and will retry for "
                         << *bh << dendl;
        }
        continue;
      }

      if (r < 0) {
        bh->error = r;
        mark_error(bh);
      } else {
        bh->bl.substr_of(bl,
                         oldpos - bh->start(),
                         bh->length());
        mark_clean(bh);
      }

      ldout(cct, 10) << "bh_read_finish read " << *bh << dendl;

      ob->try_merge_bh(bh);
    }
  }

  // called with lock held.
  ldout(cct, 20) << "finishing waiters " << ls << dendl;

  finish_contexts(cct, ls, err);
  retry_waiting_reads();

  --reads_outstanding;
  read_cond.Signal();
}

void FileCacher::bh_write_commit(int64_t poolid, Object *pOb, loff_t start,
                                 uint64_t length, ceph_tid_t tid, int r)
{
  assert(lock.is_locked());
  sobject_t oid = pOb->get_soid();
  ldout(cct, 7) << "bh_write_commit "
                << oid
                << " tid " << tid
                << " " << start << "~" << length
                << " returned " << r
                << dendl;
  /*ofs<< "bh_write_commit "
                << oid
                << " tid " << tid
                << " " << start << "~" << length
                << " returned " << r
                << std::endl;
  */
  if (find(dirty_record_map.begin(),dirty_record_map.end(),pOb->oset)==dirty_record_map.end() ||
    pOb->oset->hash_map_objects.count(oid) == 0) {//changed by li.jiebj
    ldout(cct, 7) << "bh_write_commit no object cache" << dendl;
  } else {
    Object *ob = pOb;//->oset->hash_map_objects[oid];
    int was_dirty_or_tx = ob->oset->dirty_or_tx;

    if (!ob->exists) {
      ldout(cct, 10) << "bh_write_commit marking exists on " << *ob << dendl;
      ob->exists = true;

      if (writeback_handler.may_copy_on_write(ob->get_oid(), start, length, ob->get_snap())) {
        ldout(cct, 10) << "bh_write_commit may copy on write, clearing complete on " << *ob << dendl;
        ob->complete = false;
      }
    }

    // apply to bh's!
    for (map<loff_t, BufferHead*>::iterator p = ob->data_lower_bound(start);
         p != ob->data.end();
         ++p) {
      BufferHead *bh = p->second;

      if (bh->start() > start + (loff_t)length)
        break;

      if (bh->start() < start &&
          bh->end() > start + (loff_t)length) {
        ldout(cct, 20) << "bh_write_commit skipping " << *bh << dendl;
        continue;
      }

      // make sure bh is tx
      if (!bh->is_tx()) {
        ldout(cct, 10) << "bh_write_commit skipping non-tx " << *bh << dendl;
        continue;
      }

      // make sure bh tid matches
      if (bh->last_write_tid != tid) {
        assert(bh->last_write_tid > tid);
        ldout(cct, 10) << "bh_write_commit newer tid on " << *bh << dendl;
        continue;
      }

      if (r >= 0) {
        // ok!  mark bh clean and error-free
        mark_clean(bh);
        if (pOb->oset->is_dirty_max)
        {
          pOb->oset->dirty_wait_mutex.Lock();
          pOb->oset->dirty_wait_cond.Signal();
          pOb->oset->dirty_wait_mutex.Unlock();
        }

        ldout(cct, 10) << "bh_write_commit clean " << *bh << dendl;
      } else {
        mark_dirty(bh);
        ldout(cct, 10) << "bh_write_commit marking dirty again due to error "
                       << *bh << " r = " << r << " " << cpp_strerror(-r)
                       << dendl;
      }
    }

    // update last_commit.
    assert(ob->last_commit_tid < tid);
    ob->last_commit_tid = tid;
    
    /*for(map< ceph_tid_t, list<Context*> >::iterator it = ob->waitfor_commit.begin();it !=ob->waitfor_commit.end();it++)
    {
      ofs<<"bh_write_commit1: ob:"<<ob<<" tid "<<tid<<" last_write_tid:"<<it->first <<" waiter_cnt "<<it->second.size()<<std::endl;
    }*/

    // waiters?
    list<Context*> ls;
    if (ob->waitfor_commit.count(tid)) {
      //ofs<<"bh_write_commit: ob:"<<ob<<" waiters tid - "<<tid<<std::endl;
      ls.splice(ls.begin(), ob->waitfor_commit[tid]);
      ob->waitfor_commit.erase(tid);
    }

    // is the entire object set now clean and fully committed?
    ObjectSet *oset = ob->oset;
    

    if (flush_set_callback &&
        was_dirty_or_tx > 0 &&
        oset->dirty_or_tx == 0) {        // nothing dirty/tx
      /*for(map< ceph_tid_t, list<Context*> >::iterator it = ob->waitfor_commit.begin();it !=ob->waitfor_commit.end();it++)
      {
        ofs<<"bh_write_commit2: ob:"<<ob<<" tid "<<tid<<" last_write_tid:"<<it->first <<" waiter_cnt "<<it->second.size()<<std::endl;
      }*/
      flush_set_callback(flush_set_callback_arg, oset);
    }
    ob->put();
    if (!ls.empty())
      finish_contexts(cct, ls, r);
  }
}

void FileCacher::trim(ObjectSet *oset)
{
  assert(lock.is_locked());
  //ldout(cct, 10) << "trim  start: bytes: max " << max_size << "  clean " << get_stat_clean()
  //   << ", objects: max " << max_objects << " current " << ob_lru.lru_get_size()
  //   << dendl;
  INITCEPHLOG();
  WCEPHLOG(3581);
  while (get_stat_clean() > 0 && (uint64_t) get_stat_clean()+(uint64_t)get_stat_tx()+(uint64_t)get_stat_rx()+ (uint64_t)get_stat_dirty()  > max_size) {
    BufferHead *bh = static_cast<BufferHead*>(bh_lru_rest.lru_expire());
    if (!bh)
      break;

    //ldout(cct, 10) << "trim trimming " << *bh << dendl;
    assert(bh->is_clean() || bh->is_zero());

    Object *ob = bh->ob;
    bh_remove(ob, bh);
    //lpcur_list_delete_bh->push_back(bh);
    releasebh(bh);

    if (ob->complete) {
      //ldout(cct, 10) << "trim clearing complete on " << *ob << dendl;
      ob->complete = false;
    }
  }
  WCEPHLOG(3582);
  while (oset->ob_lru.lru_get_size() > max_objects) {
    Object *ob = static_cast<Object*>(oset->ob_lru.lru_expire());
    if (!ob)
      break;

    //ldout(cct, 10) << "trim trimming " << *ob << dendl;
    WCEPHLOG(3583);
    close_object(ob);
    WCEPHLOG(3584);
  }
  WCEPHLOG(3585);
  //ldout(cct, 10) << "trim finish:  max " << max_size << "  clean " << get_stat_clean()
  //   << ", objects: max " << max_objects << " current " << ob_lru.lru_get_size()
  //   << dendl;
}





bool FileCacher::flush_set(ObjectSet *oset, vector<ObjectExtent>& exv, Context *onfinish)
{
  assert(lock.is_locked());
  assert(onfinish != NULL);
  if (oset->hash_map_objects.empty()) {
    ldout(cct, 10) << "flush_set on " << oset << " dne" << dendl;
    onfinish->complete(0);
    return true;
  }

  ldout(cct, 10) << "flush_set " << oset << " on " << exv.size()
                 << " ObjectExtents" << dendl;

  // we'll need to wait for all objects to flush!
  C_GatherBuilder gather(cct);

  for (vector<ObjectExtent>::iterator p = exv.begin();
       p != exv.end();
       ++p) {
    ObjectExtent &ex = *p;
    sobject_t soid(ex.oid, CEPH_NOSNAP);
    if (oset->hash_map_objects.count(soid) == 0)
      continue;
    Object *ob = oset->hash_map_objects[soid];

    ldout(cct, 20) << "flush_set " << oset << " ex " << ex << " ob " << soid << " " << ob << dendl;

    if (!flush(ob, ex.offset, ex.length)) {
      // we'll need to gather...
      ldout(cct, 10) << "flush_set " << oset << " will wait for ack tid "
                     << ob->last_write_tid << " on " << *ob << dendl;
      ob->waitfor_commit[ob->last_write_tid].push_back(gather.new_sub());
    }
  }

  return _flush_set_finish(&gather, onfinish);
}


uint64_t FileCacher::release_all()
{
  assert(lock.is_locked());
  ldout(cct, 10) << "release_all" << dendl;
  uint64_t unclean = 0;
#if 0
  vector<ceph::unordered_map<sobject_t, Object*> >::iterator i = objects.begin();
  while (i != objects.end()) {
    ceph::unordered_map<sobject_t, Object*>::iterator p = i->begin();
    while (p != i->end()) {
      ceph::unordered_map<sobject_t, Object*>::iterator n = p;
      ++n;

      Object *ob = p->second;

      loff_t o_unclean = release(ob);
      unclean += o_unclean;

      if (o_unclean)
        ldout(cct, 10) << "release_all " << *ob
                       << " has " << o_unclean << " bytes left"
                       << dendl;
      p = n;
    }
    ++i;
  }
#else

  BufferHead *pBh;
  // ÊÍ·Åbh_rest_lru
  uint64_t rest_cnt = bh_lru_rest.lru_get_size();
  while ((pBh = static_cast<BufferHead*>(bh_lru_rest.lru_get_next_expire())) != 0  )
  {
    //´ÓlruÖÐÉ¾³ý
    bh_lru_rest.lru_remove(pBh);
    bh_stat_sub(pBh);
    if (pBh->ob->can_close()) {
      close_object(pBh->ob);
    }
    if (pBh->ob->complete)
      pBh->ob->complete = false;
    if (!pBh->ob->exists)
      pBh->ob->exists = true;
    //delete pBh;
    releasebh(pBh);
    rest_cnt--;
  }
  unclean = rest_cnt + bh_lru_dirty.lru_get_size();
#endif
  if (unclean) {
    ldout(cct, 10) << "release_all unclean " << unclean << " bytes left" << dendl;
  }

  return unclean;
}


void FileCacher::discard_set(ObjectSet *oset, vector<ObjectExtent>& exls)
{
  assert(lock.is_locked());
  if (oset->hash_map_objects.empty()) {
    ldout(cct, 10) << "discard_set on " << oset << " dne" << dendl;
    return;
  }

  ldout(cct, 10) << "discard_set " << oset << dendl;

  bool were_dirty = oset->dirty_or_tx > 0;

  async_send_wop();//add by li.jiebj
  
  for (vector<ObjectExtent>::iterator p = exls.begin();
       p != exls.end();
       ++p) {
    ldout(cct, 10) << "discard_set " << oset << " ex " << *p << dendl;
    ObjectExtent &ex = *p;
    sobject_t soid(ex.oid, CEPH_NOSNAP);
    if (oset->hash_map_objects.count(soid) == 0)
      continue;
    Object *ob = oset->hash_map_objects[soid];

    ob->discard(ex.offset, ex.length); 
  }

  // did we truncate off dirty data?
  if (flush_set_callback &&
      were_dirty && oset->dirty_or_tx == 0)
    flush_set_callback(flush_set_callback_arg, oset);
  //add by li.jiebj
  oset->unreg();
  oset->filereadaccess.remove_self();
  oset->filewriteaccess.remove_self();
  //add by li.jiebj
}

void FileCacher::flusher_entry()
{
  ldout(cct, 10) << "flusher start" << dendl;
  lock.Lock();
  while (!flusher_stop) {
    loff_t all = get_stat_tx() + get_stat_rx() + get_stat_clean() + get_stat_dirty();
    /*ofs << "flusher "
         << all << " / " << max_size << ":  "
         << get_stat_tx() << " tx, "
         << get_stat_rx() << " rx, "
         << get_stat_clean() << " clean, "
         << get_stat_dirty() << " dirty ("
         << target_dirty << " target, "
         << max_dirty << " max)"
         << std::endl;
    */

    // check tail of lru for old dirty items
    utime_t cutoff = ceph_clock_now(cct);
    cutoff -= max_dirty_age;
    int max = cct->_conf->client_oc_max_flush_under_lock;
    uint64_t beginstamp = get_jiffies();
    switch(cct->_conf->client_dirty_flush_mode)
    {
      case 0: //取脏数据链中的数据进行刷新
      {
        BufferHead *bh = NULL;
        while ((bh = static_cast<BufferHead*>(bh_lru_dirty.lru_get_next_expire())) != 0 &&
         (bh->last_write < cutoff || bh->length() == ((Inode*)bh->ob->oset->parent)->layout.fl_object_size) &&
         max-- > 0) 
        {
          ldout(cct, 10) << "flusher flushing aged dirty bh " << *bh << dendl;
          bh_write(bh);
        }
        break;
      }
      case 1:// 每次刷新脏数据最多的oset中的一个BH，刷新5次
      {
        //ofs<<"flusher dirty_record_map size: "<<dirty_record_map.size()<<std::endl;
        for(;max != 0;max--)
        {
          if(dirty_record_map.empty()){
            break;
          }
          dirty_record_map.sort(DirtyOSetDirtyCompare());
          list<ObjectSet *>::iterator osetit =dirty_record_map.begin();

          //ofs<<"flusher lock cost 1: "<<(get_jiffies()-beginstamp)/cpu_hz<<std::endl;


          (*osetit)->dirty_bh_map.sort(DirtyBHDirtyCompare());
          list<BufferHead *>::iterator bhit =(*osetit)->dirty_bh_map.begin();

          //ofs<<"flusher lock cost 2: "<<(get_jiffies()-beginstamp)/cpu_hz<<std::endl;
          BufferHead *bh = *bhit;
          //刷新一个

          if(bh!=NULL && (bh->last_write < cutoff || 
            bh->length() == ((Inode*)bh->ob->oset->parent)->layout.fl_object_size) && 
            bh->lru_is_expireable()
            )
          {
            bh_write(bh);
          }
          else
            break;
        }
        break;
      }
      case 2: //废弃
      case 3: //取每个oset中的一个BH进行刷新
      {
        dirty_record_map.sort(DirtyOSetDirtyCompare());
        list<ObjectSet *>::iterator osetit =dirty_record_map.begin();
        for(; osetit!=dirty_record_map.end();osetit++){
          if((*osetit)->dirty_bh_map.empty())
            break;
          (*osetit)->dirty_bh_map.sort(DirtyBHDirtyCompare());
          list<BufferHead *>::iterator bhit =(*osetit)->dirty_bh_map.begin();
          BufferHead *bh = *bhit;
          if(bh!=NULL && (bh->last_write < cutoff || 
              bh->length() == ((Inode*)bh->ob->oset->parent)->layout.fl_object_size ) 
              && bh->lru_is_expireable()){
            bh_write(bh);
          }
          else 
            break;
          //额外处理当前文件过满的问题
          
          int cnt_ext = ((*osetit)->dirty_size - file_cache_quota*cct->_conf->client_file_dirty_threshold_level)/((Inode*)bh->ob->oset->parent)->layout.fl_object_size;

          while(cnt_ext > 0){
            bhit =(*osetit)->dirty_bh_map.begin();
            bh = *bhit;
            if(bh!=NULL && (bh->last_write < cutoff || 
                bh->length() == ((Inode*)bh->ob->oset->parent)->layout.fl_object_size ) 
                && bh->lru_is_expireable()){
              bh_write(bh);
            }
            else
              break;
	   			  cnt_ext--;
          }; 

        }
        break;
      }
      default:
        assert(0);
    }
    //ofs<<"flusher lock cost: "<<(get_jiffies()-beginstamp)/cpu_hz<<std::endl;

    
    if (!max || woplist.size()>=cct->_conf->client_oc_max_flush_under_lock ) {
      // back off the lock to avoid starving other thread
      lock.Unlock();
      async_send_wop();//add by li.jiebj
      lock.Lock();
      continue;
    }
    if (flusher_stop)
      break;
    flusher_cond.WaitInterval(cct, lock, utime_t(1, 0));
  }

  while (reads_outstanding > 0) {
    ldout(cct, 10) << "Waiting for all reads to complete. Number left: "
                   << reads_outstanding << dendl;
    read_cond.Wait(lock);
  }

  lock.Unlock();
  ldout(cct, 10) << "flusher finish" << dendl;
}

int FileCacher::writex(OSDWrite *wr, ObjectSet *oset, Mutex& wait_on_lock,
                       Context *onfreespace)
{
  INITCEPHLOG();
  assert(lock.is_locked());
  utime_t now = ceph_clock_now(cct);
  uint64_t bytes_written = 0;
  uint64_t bytes_written_in_flush = 0;

  for (vector<ObjectExtent>::iterator ex_it = wr->extents.begin();
       ex_it != wr->extents.end();
       ++ex_it) {
    // get object cache
    sobject_t soid(ex_it->oid, CEPH_NOSNAP);
    Object *o = get_object(soid, oset, ex_it->oloc, ex_it->truncate_size, ex_it->fileoff, oset->truncate_seq);
    // map it all into a single bufferhead.
    BufferHead *bh = o->map_write(wr);
    bh->snapc = wr->snapc;

    bytes_written += bh->length();
    if (bh->is_tx()) {
      bytes_written_in_flush += bh->length();
    }

    // adjust buffer pointers (ie "copy" data into my cache)
    // this is over a single ObjectExtent, so we know that
    //  - there is one contiguous bh
    //  - the buffer frags need not be (and almost certainly aren't)
    // note: i assume striping is monotonic... no jumps backwards, ever!
    loff_t opos = ex_it->offset;
    for (vector<pair<uint64_t, uint64_t> >::iterator f_it = ex_it->buffer_extents.begin();
         f_it != ex_it->buffer_extents.end();
         ++f_it) {
      //ldout(cct, 10) << "writex writing " << f_it->first << "~" << f_it->second << " into " << *bh << " at " << opos << dendl;
      uint64_t bhoff = bh->start() - opos;
      assert(f_it->second <= bh->length() - bhoff);

      // get the frag we're mapping in
      bufferlist frag;
      frag.substr_of(wr->bl,
                     f_it->first, f_it->second);
      // keep anything left of bhoff
      bufferlist newbl;
      if (bhoff)
        newbl.substr_of(bh->bl, 0, bhoff);
      newbl.claim_append(frag);
      bh->bl.swap(newbl);

      opos += f_it->second;
    }
    // ok, now bh is dirty.
    mark_dirty(bh);
    touch_bh(bh);
    bh->last_write = now;

    o->try_merge_bh(bh);
  }
  if (perfcounter) {
    perfcounter->inc(l_objectcacher_data_written, bytes_written);
    if (bytes_written_in_flush) {
      perfcounter->inc(l_objectcacher_overwritten_in_flush,
                       bytes_written_in_flush);
    }
  }

  //delete wr;
  wr->Release();
  free_osdwrite_list.releaseobj(wr);

  //verify_stats();
  trim(oset);
  return 0;
}
void FileCacher::delete_bh_entry()
{
  std::list<bufferptr> *lptmp_list_delete_ptr;
  std::list<bufferptr>::iterator ilistpos;
  while (!delete_bh_stoped) {
    if (delete_bh_index < delete_bh_count) {
      lock.Lock();
      lptmp_list_delete_ptr = lpcur_list_delete_ptr;
      idelete_bh_index++;
      idelete_bh_index %= 2;
      lpcur_list_delete_ptr = &_list_delete_ptr[idelete_bh_index];
      lock.Unlock();
      delete_bh_index = delete_bh_count;
      lptmp_list_delete_ptr->clear();
    }
    usleep(5000);
  }
}
void FileCacher::async_send_op()
{

  while (!is_asend_stopped) {
    std::list<BufferHead* > tmplist;
    tmplist.clear();
    if (!oplist.empty()) {

      async_send_lock.Lock();
      tmplist.swap(oplist);
      assert(!tmplist.empty());
      assert(oplist.empty());
      async_send_lock.Unlock();

      for (std::list<BufferHead* >::iterator it = tmplist.begin(); it != tmplist.end(); it++) {
        BufferHead *bh = *it;
        C_ReadFinish *onfinish = new C_ReadFinish(this, bh->ob, bh->last_read_tid,
            bh->start(), bh->length());
        writeback_handler.read(bh->ob->get_oid(), bh->ob->get_oloc(),
                               bh->start(), bh->length(), bh->ob->get_snap(),
                               &onfinish->bl, bh->ob->truncate_size, bh->ob->truncate_seq,
                               onfinish);
      }
    }
    usleep(100);
  }
}

void FileCacher::bh_read(BufferHead *bh)
{
  assert(lock.is_locked());
  ldout(cct, 7) << "bh_read on " << *bh << " outstanding reads "
                << reads_outstanding << dendl;

  mark_rx(bh);
  bh->last_read_tid = ++last_read_tid;
  async_send_lock.Lock();
  oplist.push_back(bh);
  async_send_lock.Unlock();

  ++reads_outstanding;
}
int FileCacher::_readx(OSDRead *rd, ObjectSet *oset, Context *onfinish,
                       bool external_call)
{
  assert(lock.is_locked());
  bool success = true;
  int error = 0;
  list<BufferHead*> hit_ls;
  uint64_t bytes_in_cache = 0;
  uint64_t bytes_not_in_cache = 0;
  uint64_t total_bytes_read = 0;
  map<uint64_t, bufferlist> stripe_map;  // final buffer offset -> substring
  INITCEPHLOG();
  //ldout(cct,0) << "_readx object ino"<<oset->ino.val<<dendl;
  //printf("_readx object ino:%lld\n",oset->ino.val);
  for (vector<ObjectExtent>::iterator ex_it = rd->extents.begin();
       ex_it != rd->extents.end();
       ++ex_it) {
    ldout(cct, 10) << "readx " << *ex_it << dendl;
    total_bytes_read += ex_it->length;

    // get Object cache
    sobject_t soid(ex_it->oid, rd->snap);
    WCEPHLOG(351);
    Object *o = get_object(soid, oset, ex_it->oloc, ex_it->truncate_size, ex_it->fileoff, oset->truncate_seq);
    touch_ob(o);
    WCEPHLOG(352);
    // does not exist and no hits?
    if (oset->return_enoent && !o->exists) {
      // WARNING: we can only meaningfully return ENOENT if the read request
      // passed in a single ObjectExtent.  Any caller who wants ENOENT instead of
      // zeroed buffers needs to feed single extents into readx().
      assert(rd->extents.size() == 1);
      ldout(cct, 10) << "readx  object !exists, 1 extent..." << dendl;

      // should we worry about COW underneaeth us?
      if (writeback_handler.may_copy_on_write(soid.oid, ex_it->offset, ex_it->length, soid.snap)) {
        ldout(cct, 20) << "readx  may copy on write" << dendl;
        bool wait = false;
        for (map<loff_t, BufferHead*>::iterator bh_it = o->data.begin();
             bh_it != o->data.end();
             ++bh_it) {
          BufferHead *bh = bh_it->second;
          if (bh->is_dirty() || bh->is_tx()) {
            ldout(cct, 10) << "readx  flushing " << *bh << dendl;
            wait = true;
            if (bh->is_dirty())
              bh_write(bh);
          }
        }
        if (wait) {
          ldout(cct, 10) << "readx  waiting on tid " << o->last_write_tid << " on " << *o << dendl;
          //ofs << "readx  waiting on tid " << o->last_write_tid << " on " << *o << std::endl;
          o->waitfor_commit[o->last_write_tid].push_back(new C_RetryRead(this, rd, oset, onfinish));
          // FIXME: perfcounter!
          return 0;
        }
      }

      // can we return ENOENT?
      bool allzero = true;
      for (map<loff_t, BufferHead*>::iterator bh_it = o->data.begin();
           bh_it != o->data.end();
           ++bh_it) {
        ldout(cct, 20) << "readx  ob has bh " << *bh_it->second << dendl;
        if (!bh_it->second->is_zero() && !bh_it->second->is_rx()) {
          allzero = false;
          break;
        }
      }
      if (allzero) {
        ldout(cct, 10) << "readx  ob has all zero|rx, returning ENOENT" << dendl;
        //delete rd;
        rd->Release();
        free_osdread_list.releaseobj(rd);

        return -ENOENT;
      }
    }
    WCEPHLOG(353);
    // map extent into bufferheads
    map<loff_t, BufferHead*> hits, missing, rx, errors;
    o->map_read(rd, hits, missing, rx, errors);
    WCEPHLOG(354);
    if (external_call) {
      // retry reading error buffers
      missing.insert(errors.begin(), errors.end());
    } else {
      // some reads had errors, fail later so completions
      // are cleaned up up properly
      // TODO: make read path not call _readx for every completion
      hits.insert(errors.begin(), errors.end());
    }

    if (!missing.empty() || !rx.empty()) {
      // read missing
      for (map<loff_t, BufferHead*>::iterator bh_it = missing.begin();
           bh_it != missing.end();
           ++bh_it) {
        uint64_t rx_bytes = static_cast<uint64_t>(
                              stat_rx + bh_it->second->length());
        if (!waitfor_read.empty() || rx_bytes > max_size) {
          // cache is full with concurrent reads -- wait for rx's to complete
          // to constrain memory growth (especially during copy-ups)
          if (success) {
            ldout(cct, 10) << "readx missed, waiting on cache to complete "
                           << waitfor_read.size() << " blocked reads, "
                           << (MAX(rx_bytes, max_size) - max_size)
                           << " read bytes" << dendl;
            waitfor_read.push_back(new C_RetryRead(this, rd, oset, onfinish));
          }

          bh_remove(o, bh_it->second);
          //delete bh_it->second;
          releasebh(bh_it->second);
        } else {
          bh_read(bh_it->second);
          if (rd->bl == NULL) { //add by li.jiebj
            BufferHead *bh = bh_it->second;
            bh->move_to(&bh->ob->oset->bh_lru_readahead);
            //bh->ob->try_merge_bh(bh);// 新创建的没有必要合并,会在bh_read_finish 时调用一次
          }
          if (success && onfinish) {
            ldout(cct, 10) << "readx missed, waiting on " << *bh_it->second
                           << " off " << bh_it->first << dendl;
            bh_it->second->waitfor_read[bh_it->first].push_back( new C_RetryRead(this, rd, oset, onfinish) );
          }
        }
        bytes_not_in_cache += bh_it->second->length();
        success = false;
      }

      // bump rx
      for (map<loff_t, BufferHead*>::iterator bh_it = rx.begin();
           bh_it != rx.end();
           ++bh_it) {
        touch_bh(bh_it->second);        // bump in lru, so we don't lose it.





        if (success && onfinish) {
          ldout(cct, 10) << "readx missed, waiting on " << *bh_it->second
                         << " off " << bh_it->first << dendl;
          bh_it->second->waitfor_read[bh_it->first].push_back( new C_RetryRead(this, rd, oset, onfinish) );
        }
        bytes_not_in_cache += bh_it->second->length();
        success = false;
      }


    } else {
      assert(!hits.empty());

      // make a plain list
      WCEPHLOG(3541);
      for (map<loff_t, BufferHead*>::iterator bh_it = hits.begin();
           bh_it != hits.end();
           ++bh_it) {
        ldout(cct, 10) << "readx hit bh " << *bh_it->second << dendl;
        if (bh_it->second->is_error() && bh_it->second->error)
          error = bh_it->second->error;
        hit_ls.push_back(bh_it->second);
        bytes_in_cache += bh_it->second->length();
      }
      WCEPHLOG(3542);
      // create reverse map of buffer offset -> object for the eventual result.
      // this is over a single ObjectExtent, so we know that
      //  - the bh's are contiguous
      //  - the buffer frags need not be (and almost certainly aren't)
      loff_t opos = ex_it->offset;
      map<loff_t, BufferHead*>::iterator bh_it = hits.begin();
      assert(bh_it->second->start() <= opos);
      uint64_t bhoff = opos - bh_it->second->start();
      vector<pair<uint64_t, uint64_t> >::iterator f_it = ex_it->buffer_extents.begin();
      uint64_t foff = 0;
      WCEPHLOG(3543);
      while (1) {
        BufferHead *bh = bh_it->second;
        assert(opos == (loff_t)(bh->start() + bhoff));

        uint64_t len = MIN(f_it->second - foff, bh->length() - bhoff);
        ldout(cct, 10) << "readx rmap opos " << opos
                       << ": " << *bh << " +" << bhoff
                       << " frag " << f_it->first << "~" << f_it->second << " +" << foff << "~" << len
                       << dendl;

        bufferlist bit;  // put substr here first, since substr_of clobbers, and
        // we may get multiple bh's at this stripe_map position
        if (bh->is_zero()) {
          bufferptr bp(len);
          bp.zero();
          stripe_map[f_it->first].push_back(bp);
        } else {
          bit.substr_of(bh->bl,
                        opos - bh->start(),
                        len);
          stripe_map[f_it->first].claim_append(bit);
        }
        WCEPHLOG(3544);
        opos += len;
        bhoff += len;
        foff += len;
        if (opos == bh->end()) {
          ++bh_it;
          bhoff = 0;
        }
        if (foff == f_it->second) {
          ++f_it;
          foff = 0;
        }
        if (bh_it == hits.end()) break;
        if (f_it == ex_it->buffer_extents.end())
          break;
      }
      assert(f_it == ex_it->buffer_extents.end());
      assert(opos == (loff_t)ex_it->offset + (loff_t)ex_it->length);
    }
    WCEPHLOG(3545);
  }

  if (!success) {
    if (perfcounter && external_call) {
      perfcounter->inc(l_objectcacher_data_read, total_bytes_read);
      perfcounter->inc(l_objectcacher_cache_bytes_miss, bytes_not_in_cache);
      perfcounter->inc(l_objectcacher_cache_ops_miss);
    }
    if (onfinish) {
      ldout(cct, 20) << "readx defer " << rd << dendl;
    } else {
      ldout(cct, 20) << "readx drop " << rd << " (no complete, but no waiter)" << dendl;
      //delete rd;
      rd->Release();
      free_osdread_list.releaseobj(rd);

    }
    return 0;  // wait!
  }
  if (perfcounter && external_call) {
    perfcounter->inc(l_objectcacher_data_read, total_bytes_read);
    perfcounter->inc(l_objectcacher_cache_bytes_hit, bytes_in_cache);
    perfcounter->inc(l_objectcacher_cache_ops_hit);
  }

  // no misses... success!  do the read.
  assert(!hit_ls.empty());
  ldout(cct, 10) << "readx has all buffers" << dendl;
  WCEPHLOG(355);
  // ok, assemble into result buffer.
  uint64_t pos = 0;
  if (rd->bl && !error) {
    rd->bl->clear();
    for (map<uint64_t, bufferlist>::iterator i = stripe_map.begin();
         i != stripe_map.end();
         ++i) {
      assert(pos == i->first);
      ldout(cct, 10) << "readx  adding buffer len " << i->second.length() << " at " << pos << dendl;
      pos += i->second.length();
      rd->bl->claim_append(i->second);


      assert(rd->bl->length() == pos);
    }
    WCEPHLOG(356);
    // bump hits in lru
    for (list<BufferHead*>::iterator bhit = hit_ls.begin();
         bhit != hit_ls.end();
         ++bhit) {
      if ((*bhit)->is_dirty()) {
        bh_lru_dirty.lru_touch(*bhit);
      } else {
        if (oset->last_pos + total_bytes_read >= (*bhit)->ob->foff + (*bhit)->end()
           && (*bhit)->get_lru() == &((*bhit)->ob->oset->bh_lru_readahead)) {
          (*bhit)->move_to(&bh_lru_rest);
          (*bhit)->ob->try_merge_bh(*bhit);
        }
      }
    }


    ldout(cct, 10) << "readx  result is " << rd->bl->length() << dendl;
  } else {
    ldout(cct, 10) << "readx  no bufferlist ptr (readahead?), done." << dendl;
    map<uint64_t, bufferlist>::reverse_iterator i = stripe_map.rbegin();
    pos = i->first + i->second.length();
  }

  // done with read.
  int ret = error ? error : pos;
  //ldout(cct, 20) << "readx done " << rd << " " << ret << dendl;
  //assert(pos <= (uint64_t) INT_MAX);
  WCEPHLOG(357);
  //delete rd;
  rd->Release();
  free_osdread_list.releaseobj(rd);
  WCEPHLOG(358);
  trim(oset);

  return ret;
}
void FileCacher::bh_remove(Object *ob, BufferHead *bh)
{
  assert(lock.is_locked());
  ldout(cct, 30) << "bh_remove " << *ob << " " << *bh << dendl;
  ob->remove_bh(bh);
  bh->remove_self();
  if (bh->is_dirty() || bh->is_tx())
    dirty_or_tx_bh.erase(bh);
  bh_stat_sub(bh);
}
ObjectCacher::BufferHead *FileCacher::allocbh(Object *ob) {
  BufferHead *tmpbh = free_bh_list.allocobj();
  tmpbh->InitObj(ob);
  return tmpbh;
}
void FileCacher::releasebh(BufferHead *bh) {
  bufferlist &bl = bh->bl;
  const std::list<bufferptr> &buffers = bl.buffers();
  std::list<bufferptr>::const_iterator curbuf = buffers.begin();
  for (; curbuf != buffers.end(); ++curbuf) {
    lpcur_list_delete_ptr->push_back(bufferptr(*curbuf));
  }
  bh->Release();
  free_bh_list.releaseobj(bh);
  ++delete_bh_count;
}



void FileCacher::bh_set_state(BufferHead *bh, int s)
{
  assert(lock.is_locked());
  int state = bh->get_state();
  // move between lru lists?
  if (s == BufferHead::STATE_DIRTY && state != BufferHead::STATE_DIRTY) {
    //bh_lru_rest.lru_remove(bh);
    //bh_lru_dirty.lru_insert_top(bh);
    bh->move_to(&bh_lru_dirty);
    bh->reg_dirty();//add by lijie
  } else if (s != BufferHead::STATE_DIRTY && state == BufferHead::STATE_DIRTY) {
    bh_lru_dirty.lru_remove(bh);
    bh_lru_rest.lru_insert_top(bh);
    bh->unreg_dirty(); //add by lijie
  }
  
  
  
  if ((s == BufferHead::STATE_TX ||
       s == BufferHead::STATE_DIRTY) &&
      state != BufferHead::STATE_TX &&
      state != BufferHead::STATE_DIRTY) {
    dirty_or_tx_bh.insert(bh);

  } else if ((state == BufferHead::STATE_TX ||
        state == BufferHead::STATE_DIRTY) &&
       s != BufferHead::STATE_TX &&
       s != BufferHead::STATE_DIRTY) {
    dirty_or_tx_bh.erase(bh);
  }

  if (s != BufferHead::STATE_ERROR && bh->get_state() == BufferHead::STATE_ERROR) {
    bh->error = 0;
  }

  // set state
  bh_stat_sub(bh);
  bh->set_state(s);
  bh_stat_add(bh);
}


void FileCacher::bh_write(BufferHead *bh)
{
  assert(lock.is_locked());
  ldout(cct, 7) << "bh_write " << *bh << dendl;
  Inode *in =  (Inode *)bh->ob->oset->parent;
  if( bh->length()==in->layout.fl_object_size){
    bh->ob->get();
    //async_wsend_lock.Lock();
    //ceph_tid_t tid = writeback_handler.get_next_tid();
    //bh->ob->last_write_tid = tid;
    //bh->last_write_tid = tid;
    woplist.push_back(bh);
    /*ofs<<" bh_lru_dirty length: "<<bh_lru_dirty.lru_get_size()
       <<" bh->length " <<bh->length()
       << " woplist lenght: "<<woplist.size()<<std::endl; */
    //async_wsend_lock.Unlock(); 
    if (perfcounter) {
      perfcounter->inc(l_objectcacher_data_flushed, bh->length());
    }
    
    mark_tx(bh);

  }else{
    bh->ob->get();

    // finishers
    C_WriteCommit *oncommit = new C_WriteCommit(this, bh->ob->oloc.pool,
        bh->ob, bh->start(), bh->length());
    // go
    ceph_tid_t tid = writeback_handler.write(bh->ob->get_oid(), bh->ob->get_oloc(),
        bh->start(), bh->length(),
        bh->snapc, bh->bl, bh->last_write,
        bh->ob->truncate_size, bh->ob->truncate_seq,
        oncommit);
    ldout(cct, 20) << " tid " << tid << " on " << bh->ob->get_oid() << dendl;

    // set bh last_write_tid
    oncommit->tid = tid;
    bh->ob->last_write_tid = tid;
    bh->last_write_tid = tid;

    if (perfcounter) {
      perfcounter->inc(l_objectcacher_data_flushed, bh->length());
    }
    mark_tx(bh);
  }
}
void FileCacher::async_send_wop()
{

  async_wsend_lock.Lock();
  for(std::list<BufferHead* >::iterator it = woplist.begin();it!=woplist.end();it++){
    BufferHead *bh = *it;
    if(!bh
    	||!bh->is_tx()
    	|| find(dirty_record_map.begin(),dirty_record_map.end(),bh->ob->oset)==dirty_record_map.end()
    	){
      continue;
    }
      
      // finishers
    C_WriteCommit *oncommit = new C_WriteCommit(this, bh->ob->oloc.pool,
      bh->ob, bh->start(), bh->length());
      // go
    ceph_tid_t tid = writeback_handler.write(bh->ob->get_oid(), bh->ob->get_oloc(),
      bh->start(), bh->length(),
      bh->snapc, bh->bl, bh->last_write,
      bh->ob->truncate_size, bh->ob->truncate_seq,
      oncommit);
    ldout(cct, 20) << " tid " << tid << " on " << bh->ob->get_oid() << dendl;

    // set bh last_write_tid
    oncommit->tid = tid;
    bh->ob->last_write_tid = tid;
    bh->last_write_tid = tid;

  } 

  woplist.clear();
  assert(woplist.size()==0);
  async_wsend_lock.Unlock();
}
// flush.  non-blocking, takes callback.
// returns true if already flushed
bool FileCacher::flush_set(ObjectSet *oset, Context *onfinish)
{
  assert(lock.is_locked());
  assert(onfinish != NULL);
  if (oset->objects.empty()) {
    ldout(cct, 10) << "flush_set on " << oset << " dne" << dendl;
    onfinish->complete(0);
    return true;
  }

  ldout(cct, 10) << "flush_set " << oset << dendl;

  // we'll need to wait for all objects to flush!
  C_GatherBuilder gather(cct);
  set<Object*> waitfor_commit;

  set<BufferHead*>::iterator next, it;
  next = it = dirty_or_tx_bh.begin();
  while (it != dirty_or_tx_bh.end()) {
    ++next;
    BufferHead *bh = *it;
    if(bh->last_write_tid == 0){ //add by li.jiebj
      async_send_wop();
    }
    waitfor_commit.insert(bh->ob);

    if (bh->is_dirty())
      bh_write(bh);

    it = next;
  }

  for (set<Object*>::iterator i = waitfor_commit.begin();
       i != waitfor_commit.end(); ++i) {
    Object *ob = *i;

    // we'll need to gather...
    ldout(cct, 10) << "flush_set " << oset << " will wait for ack tid "
             << ob->last_write_tid
             << " on " << *ob
             << dendl;
    if(ob->last_write_tid ==0)
    {
      async_send_wop();
    } 
    ob->waitfor_commit[ob->last_write_tid].push_back(gather.new_sub());
  }

  return _flush_set_finish(&gather, onfinish);
}
