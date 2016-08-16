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


#include "Filer.h"
#include "osd/OSDMap.h"
#include "Striper.h"

#include "messages/MOSDOp.h"
#include "messages/MOSDOpReply.h"
#include "messages/MOSDMap.h"

#include "msg/Messenger.h"

#include "include/Context.h"

#include "common/Finisher.h"
#include "common/config.h"

#define dout_subsys ceph_subsys_filer
#undef dout_prefix
#define dout_prefix _prefix(*_dout, __func__)

ostream& Filer::_prefix(ostream& os, const char *func)
{
  os << objecter->messenger->get_myname() << ".filer ";
  if (is_optimized()) {
    bool release_lock(false);
    if (!optimized_lock.is_locked_by_me()) {
      release_lock = true;
      optimized_lock.Lock();
    }

    os << func << " filer_ops[" << total_filer_ops.read() 
       << "," << processing_filer_ops.read()
       << "," << waiting_ops.size()
       << "] obj_ops[" << total_obj_ops.read()
       << "," << optimized_ops
       << "," << waiting_obj_ops.read() << "] ";
    if (release_lock)
      optimized_lock.Unlock();
  }

  return os;
}

class Filer::C_Probe : public Context {
public:
  Filer *filer;
  Probe *probe;
  object_t oid;
  uint64_t size;
  utime_t mtime;
  C_Probe(Filer *f, Probe *p, object_t o) : filer(f), probe(p), oid(o), size(0) {}
  void finish(int r) {
    if (r == -ENOENT) {
      r = 0;
      assert(size == 0);
    }

    bool probe_complete;
    {
      probe->lock.Lock();
      if (r != 0) {
        probe->err = r;
      }

      probe_complete = filer->_probed(probe, oid, size, mtime);
      assert(!probe->lock.is_locked_by_me());
    }
    if (probe_complete) {
      probe->onfinish->complete(probe->err);
      delete probe;
    }
  }  
};

int Filer::probe(inodeno_t ino,
		 ceph_file_layout *layout,
		 snapid_t snapid,
		 uint64_t start_from,
		 uint64_t *end,           // LB, when !fwd
		 utime_t *pmtime,
		 bool fwd,
		 int flags,
		 Context *onfinish) 
{
  ldout(cct, 10) << "probe " << (fwd ? "fwd ":"bwd ")
	   << hex << ino << dec
	   << " starting from " << start_from
	   << dendl;

  assert(snapid);  // (until there is a non-NOSNAP write)

  Probe *probe = new Probe(ino, *layout, snapid, start_from, end, pmtime, flags, fwd, onfinish);
  
  // period (bytes before we jump unto a new set of object(s))
  uint64_t period = (uint64_t)layout->fl_stripe_count * (uint64_t)layout->fl_object_size;
  
  // start with 1+ periods.
  probe->probing_len = period;
  if (probe->fwd) {
    if (start_from % period)
      probe->probing_len += period - (start_from % period);
  } else {
    assert(start_from > *end);
    if (start_from % period)
      probe->probing_len -= period - (start_from % period);
    probe->probing_off -= probe->probing_len;
  }
  
  probe->lock.Lock();
  _probe(probe);
  assert(!probe->lock.is_locked_by_me());

  return 0;
}


/**
 * probe->lock must be initially locked, this function will release it
 */
void Filer::_probe(Probe *probe)
{
  assert(probe->lock.is_locked_by_me());

  ldout(cct, 10) << "_probe " << hex << probe->ino << dec 
	   << " " << probe->probing_off << "~" << probe->probing_len 
	   << dendl;
  
  // map range onto objects
  probe->known_size.clear();
  probe->probing.clear();
  Striper::file_to_extents(cct, probe->ino, &probe->layout,
			   probe->probing_off, probe->probing_len, 0, probe->probing);
  
  std::vector<ObjectExtent> stat_extents;
  for (vector<ObjectExtent>::iterator p = probe->probing.begin();
       p != probe->probing.end();
       ++p) {
    ldout(cct, 10) << "_probe  probing " << p->oid << dendl;
    probe->ops.insert(p->oid);
    stat_extents.push_back(*p);
  }

  probe->lock.Unlock();
  for (std::vector<ObjectExtent>::iterator i = stat_extents.begin();
      i != stat_extents.end(); ++i) {
    C_Probe *c = new C_Probe(this, probe, i->oid);
    objecter->stat(i->oid, i->oloc, probe->snapid, &c->size, &c->mtime,
		   probe->flags | CEPH_OSD_FLAG_RWORDERED,
		   new C_OnFinisher(c, finisher));
  }
}

/**
 * probe->lock must be initially held, and will be released by this function.
 *
 * @return true if probe is complete and Probe object may be freed.
 */
bool Filer::_probed(Probe *probe, const object_t& oid, uint64_t size, utime_t mtime)
{
  assert(probe->lock.is_locked_by_me());

  ldout(cct, 10) << "_probed " << probe->ino << " object " << oid
	   << " has size " << size << " mtime " << mtime << dendl;

  probe->known_size[oid] = size;
  if (mtime > probe->max_mtime)
    probe->max_mtime = mtime;

  assert(probe->ops.count(oid));
  probe->ops.erase(oid);

  if (!probe->ops.empty()) {
    probe->lock.Unlock();
    return false;  // waiting for more!
  }

  if (probe->err) { // we hit an error, propagate back up
    probe->lock.Unlock();
    return true;
  }

  // analyze!
  uint64_t end = 0;

  if (!probe->fwd) {
    // reverse
    vector<ObjectExtent> r;
    for (vector<ObjectExtent>::reverse_iterator p = probe->probing.rbegin();
	 p != probe->probing.rend();
	 ++p)
      r.push_back(*p);
    probe->probing.swap(r);
  }

  for (vector<ObjectExtent>::iterator p = probe->probing.begin();
       p != probe->probing.end();
       ++p) {
    uint64_t shouldbe = p->length + p->offset;
    ldout(cct, 10) << "_probed  " << probe->ino << " object " << hex << p->oid << dec
	     << " should be " << shouldbe
	     << ", actual is " << probe->known_size[p->oid]
	     << dendl;

    if (!probe->found_size) {
      assert(probe->known_size[p->oid] <= shouldbe);

      if ((probe->fwd && probe->known_size[p->oid] == shouldbe) ||
	  (!probe->fwd && probe->known_size[p->oid] == 0 && probe->probing_off > 0))
	continue;  // keep going
      
      // aha, we found the end!
      // calc offset into buffer_extent to get distance from probe->from.
      uint64_t oleft = probe->known_size[p->oid] - p->offset;
      for (vector<pair<uint64_t, uint64_t> >::iterator i = p->buffer_extents.begin();
	   i != p->buffer_extents.end();
	   ++i) {
	if (oleft <= (uint64_t)i->second) {
	  end = probe->probing_off + i->first + oleft;
	  ldout(cct, 10) << "_probed  end is in buffer_extent " << i->first << "~" << i->second << " off " << oleft 
		   << ", from was " << probe->probing_off << ", end is " << end 
		   << dendl;
	  
	  probe->found_size = true;
	  ldout(cct, 10) << "_probed found size at " << end << dendl;
	  *probe->psize = end;
	  
	  if (!probe->pmtime)  // stop if we don't need mtime too
	    break;
	}
	oleft -= i->second;
      }
    }
    break;
  }

  if (!probe->found_size || (probe->probing_off && probe->pmtime)) {
    // keep probing!
    ldout(cct, 10) << "_probed probing further" << dendl;

    uint64_t period = (uint64_t)probe->layout.fl_stripe_count * (uint64_t)probe->layout.fl_object_size;
    if (probe->fwd) {
      probe->probing_off += probe->probing_len;
      assert(probe->probing_off % period == 0);
      probe->probing_len = period;
    } else {
      // previous period.
      assert(probe->probing_off % period == 0);
      probe->probing_len = period;
      probe->probing_off -= period;
    }
    _probe(probe);
    assert(!probe->lock.is_locked_by_me());
    return false;
  } else if (probe->pmtime) {
    ldout(cct, 10) << "_probed found mtime " << probe->max_mtime << dendl;
    *probe->pmtime = probe->max_mtime;
  }

  // done!
  probe->lock.Unlock();
  return true;
}


// -----------------------

struct PurgeRange {
  Mutex lock;
  inodeno_t ino;
  ceph_file_layout layout;
  SnapContext snapc;
  uint64_t first, num;
  utime_t mtime;
  int flags;
  Context *oncommit;
  int uncommitted;
  PurgeRange(inodeno_t i, ceph_file_layout& l, const SnapContext& sc,
	     uint64_t fo, uint64_t no, utime_t t, int fl, Context *fin) :
	  lock("Filer::PurgeRange"), ino(i), layout(l), snapc(sc),
	  first(fo), num(no), mtime(t), flags(fl), oncommit(fin),
	  uncommitted(0) {}
};

ostream& operator<<(ostream& os, const Filer::OptimizedFilerOp& op)
{
  if (Filer::OptimizedFilerOp::PURGE_RANGE == op.type) {
    os << "purge_range(ino " << op.ino 
       << ", first " << op.first_obj;
  } else {
    assert(Filer::OptimizedFilerOp::TRUNCATE == op.type);
    os << "truncate(ino " << op.ino
       << ", offset " << op.offset
       << ", len " << op.len
       << ", truncate_seq " << op.truncate_seq;
  }

  os << ", num_obj " << op.num_obj
     << ", oncommit " << op.oncommit
     << ", uncommitted " << op.uncommitted
     << ", onack " << op.onack
     << ", unacked " << op.unacked
     << ")";
  return os;
}

int Filer::purge_range(inodeno_t ino,
		       ceph_file_layout *layout,
		       const SnapContext& snapc,
		       uint64_t first_obj, uint64_t num_obj,
		       utime_t mtime,
		       int flags,
		       Context *oncommit) 
{
  assert(num_obj > 0);

  if (is_optimized() && ino.val >= (1UL << 40)) { // wumq added for quick-delete
    OptimizedFilerOp *op = new OptimizedFilerOp(OptimizedFilerOp::PURGE_RANGE,
        ino, layout, snapc, mtime, flags, oncommit);
    op->first_obj = first_obj;
    op->set_num_obj(num_obj);
    total_filer_ops.inc();
    total_obj_ops.add(num_obj);
    
    optimized_lock.Lock();
    if (num_obj + optimized_ops <= max_optimized_ops ||
      0 == optimized_ops) { // let single big file through!
      processing_filer_ops.inc();
      optimized_ops += num_obj;
      ldout(cct, 10) << "submit " << *op << dendl;
      optimized_lock.Unlock();
      _optimized_purge_range(op);
    } else {
      waiting_ops.push_back(op);
      waiting_obj_ops.add(num_obj);
      ldout(cct, 10) << "queue " << *op << dendl;
      optimized_lock.Unlock();
    }
    return 0;
  }

  // single object?  easy!
  if (num_obj == 1) {
    object_t oid = file_object_t(ino, first_obj);
    const OSDMap *osdmap = objecter->get_osdmap_read();
    object_locator_t oloc = osdmap->file_to_object_locator(*layout);
    objecter->put_osdmap_read();
    objecter->remove(oid, oloc, snapc, mtime, flags, NULL, oncommit);
    return 0;
  }

  PurgeRange *pr = new PurgeRange(ino, *layout, snapc, first_obj,
				  num_obj, mtime, flags, oncommit);

  _do_purge_range(pr, 0);
  return 0;
}

struct C_PurgeRange : public Context {
  Filer *filer;
  PurgeRange *pr;
  C_PurgeRange(Filer *f, PurgeRange *p) : filer(f), pr(p) {}
  void finish(int r) {
    filer->_do_purge_range(pr, 1);
  }
};

void Filer::_do_purge_range(PurgeRange *pr, int fin)
{
  pr->lock.Lock();
  pr->uncommitted -= fin;
  ldout(cct, 10) << "_do_purge_range " << pr->ino << " objects " << pr->first << "~" << pr->num
	   << " uncommitted " << pr->uncommitted << dendl;

  if (pr->num == 0 && pr->uncommitted == 0) {
    pr->oncommit->complete(0);
    pr->lock.Unlock();
    delete pr;
    return;
  }

  std::vector<object_t> remove_oids;

  int max = 10 - pr->uncommitted;
  while (pr->num > 0 && max > 0) {
    remove_oids.push_back(file_object_t(pr->ino, pr->first));
    pr->uncommitted++;
    pr->first++;
    pr->num--;
    max--;
  }
  pr->lock.Unlock();

  // Issue objecter ops outside pr->lock to avoid lock dependency loop
  for (std::vector<object_t>::iterator i = remove_oids.begin();
      i != remove_oids.end(); ++i) {
    const object_t oid = *i;
    const OSDMap *osdmap = objecter->get_osdmap_read();
    const object_locator_t oloc = osdmap->file_to_object_locator(pr->layout);
    objecter->put_osdmap_read();
    objecter->remove(oid, oloc, pr->snapc, pr->mtime, pr->flags, NULL,
		     new C_OnFinisher(new C_PurgeRange(this, pr), finisher));
  }
}

// wumq added for quickdelete begin
void Filer::_optimized_ack(OptimizedFilerOp *op)
{
  op->lock.Lock();
  ldout(cct, 20) << *op << dendl;
  assert(op->unacked);
  --op->unacked;
  if (0 == op->unacked) {
    assert(op->onack);
    op->onack->complete(0);
    op->onack = NULL;

    if (0 == op->uncommitted) {
      op->lock.Unlock();
      total_filer_ops.dec();
      processing_filer_ops.dec();
      ldout(cct, 10) << "delete " << *op << dendl;
      delete op;
      op = NULL;
    }
  }
  if (op)
    op->lock.Unlock();
}

void Filer::_optimized_commit(OptimizedFilerOp *op)
{
  op->lock.Lock();
  ldout(cct, 20) << *op << dendl;
  assert(op->uncommitted);
  --op->uncommitted;
  if (0 == op->uncommitted) {
    if (op->oncommit) {
      op->oncommit->complete(0);
      op->oncommit = NULL;
    }

    if (0 == op->unacked) {
      op->lock.Unlock();
      total_filer_ops.dec();
      processing_filer_ops.dec();
      ldout(cct, 10) << "delete " << *op << dendl;
      delete op;
      op = NULL;
    }
  }
  if (op)
    op->lock.Unlock();

  OptimizedFilerOp *next = NULL;
  optimized_lock.Lock();
  assert (optimized_ops);
  --optimized_ops;
  total_obj_ops.dec();
  ldout(cct, 20) << dendl;
  if (waiting_ops.size()) {
    if (waiting_ops.front()->num_obj + optimized_ops <= max_optimized_ops ||
        0 == optimized_ops) { // let single big file through!
      next = waiting_ops.front();
      waiting_ops.pop_front();
      processing_filer_ops.inc();
      optimized_ops += next->num_obj;
      waiting_obj_ops.sub(next->num_obj);
      ldout(cct, 10) << "dequeue " << *next << dendl;
    }
  }
  optimized_lock.Unlock();

  if (next)
    finisher->queue(new C_OptimizedSubmit(this, next));
}

void Filer::_optimized_purge_range(OptimizedFilerOp *op)
{
  ldout(cct, 20) << *op << dendl;
  object_locator_t oloc = OSDMap::file_to_object_locator(op->layout);
  uint64_t end_obj = op->first_obj + op->num_obj;
  for (uint64_t i = op->first_obj; i < end_obj; ++i) {
    object_t oid = file_object_t(op->ino, i);
    vector<OSDOp> ops(1);
    ops[0].op.op = CEPH_OSD_OP_DELETE;
    objecter->_optimized_modify(oid, oloc, ops, op->mtime, op->snapc, op->flags, 
        op->onack ? new C_OptimizedAck(this, op) : NULL, 
        new C_OptimizedCommit(this, op));
  }
}

void Filer::_optimized_truncate(OptimizedFilerOp *op, vector<ObjectExtent>& extents)
{
  ldout(cct, 20) << *op << dendl;
  for (vector<ObjectExtent>::iterator p = extents.begin(); p != extents.end(); ++p) {
    vector<OSDOp> ops(1);
    ops[0].op.op = CEPH_OSD_OP_TRIMTRUNC;
    ops[0].op.extent.truncate_size = p->offset;
    ops[0].op.extent.truncate_seq = op->truncate_seq;
    objecter->_optimized_modify(p->oid, p->oloc, ops, op->mtime, op->snapc, op->flags,
        op->onack ? new C_OptimizedAck(this, op) : NULL,
        new C_OptimizedCommit(this, op));
  }
}

int Filer::truncate(inodeno_t ino,
    ceph_file_layout *layout,
    const SnapContext& snapc,
    uint64_t offset,
    uint64_t len,
    __u32 truncate_seq,
    utime_t mtime,
    int flags,
    Context *onack,
    Context *oncommit)
{
  vector<ObjectExtent> extents;
  Striper::file_to_extents(cct, ino, layout, offset, len, 0, extents);
   
  if (is_optimized() && ino.val >= (1UL << 40)) {
    OptimizedFilerOp *op = new OptimizedFilerOp(OptimizedFilerOp::TRUNCATE,
        ino, layout, snapc, mtime, flags, oncommit, onack);
    op->offset = offset;
    op->len = len;
    op->truncate_seq = truncate_seq;
    op->set_num_obj(extents.size());
    total_filer_ops.inc();
    total_obj_ops.add(op->num_obj);

    optimized_lock.Lock();
    if (op->num_obj + optimized_ops <= max_optimized_ops ||
      0 == optimized_ops) { // let single big file through!
      optimized_ops += op->num_obj;
      processing_filer_ops.inc();
      ldout(cct, 10) << "submit " << *op << dendl;
      optimized_lock.Unlock();
      _optimized_truncate(op, extents);
    } else {
      waiting_ops.push_back(op);
      waiting_obj_ops.add(op->num_obj);
      ldout(cct, 10) << "queue " << *op << dendl;
      optimized_lock.Unlock();
    }
  } else {
    if (extents.size() == 1) {
      vector<OSDOp> ops(1);
      ops[0].op.op = CEPH_OSD_OP_TRIMTRUNC;
      ops[0].op.extent.truncate_seq = truncate_seq;
      ops[0].op.extent.truncate_size = extents[0].offset;
      objecter->_modify(extents[0].oid, extents[0].oloc, ops, mtime, snapc, flags, onack, oncommit);
    } else {
      C_GatherBuilder gack(cct, onack);
      C_GatherBuilder gcom(cct, oncommit);
      for (vector<ObjectExtent>::iterator p = extents.begin(); p != extents.end(); ++p) {
        vector<OSDOp> ops(1);
        ops[0].op.op = CEPH_OSD_OP_TRIMTRUNC;
        ops[0].op.extent.truncate_size = p->offset;
        ops[0].op.extent.truncate_seq = truncate_seq;
        objecter->_modify(p->oid, p->oloc, ops, mtime, snapc, flags,
			      onack ? gack.new_sub():0, oncommit ? gcom.new_sub():0);
      }
      gack.activate();
      gcom.activate();
    }
  }

  return 0;
}
// wumq added for quickdelete end

