#include "include/int_types.h"
#include "common/errno.h"
#include "common/sync_filesystem.h"
#include "JournalOptimize.h"
#include "FileStore.h"
#include "chain_xattr.h"

#define DESC_FILE "append-descs"
#define TEMP_DESC_FILE "~append-descs"

#define dout_subsys ceph_subsys_jopt
#undef dout_prefix
#define dout_prefix *_dout << "jopt "

OpTracer::~OpTracer() {
  assert(ops.empty());
  dout(20) << "~Optracer " << this << dendl;
}

ostream& operator<<(ostream& os, const TracedOp& op)
{
  os << "TracedOp[(" << op.tls_num << ", " << op.trans_num << ", " << op.op_num << ")";
  if (op.type != TracedOp::UNKNOWN)
    os << ", " << op.get_type();
  if (op.temp_write_seq)
    os << ", temp_write_seq = " << op.temp_write_seq;
  if (op.append_seq)
    os << ", append_seq = " << op.append_seq;
  return os << "]";
}

void OpTracer::commit_write(OpTracerMap& tracer_map, TracedOp& op, uint64_t off, uint64_t len)
{
  uint64_t end_pos = off + len;
  
  lock.Lock();
  assert(ops.empty() || ops.back() < op);
  
  if (0 == off && end_pos >= status.size) { // full write
    if (tracer_map.temp_write_ok(len)) {
      op.type = TracedOp::FULL_WRITE;
      op.temp_write_seq = tracer_map.get_temp_write_seq();
    } else if (end_pos > status.size) {
      op.type = TracedOp::OVERLAY_WRITE;
    } else {
      op.type = TracedOp::PARTICAL_WRITE;
    }
  } else if (off >= status.size) { // append
    if (tracer_map.append_ok(len)) {
      op.type = TracedOp::APPEND;
      op.temp_write_seq = 0;
      
      for (list<TracedOp>::iterator i = ops.begin(); i != ops.end(); ++i) {
        if (TracedOp::OVERLAY_WRITE == i->type || TracedOp::RESIZE == i->type) {
          op.type = TracedOp::OVERLAY_WRITE;
        } else if (TracedOp::FULL_WRITE == i->type) {
          op.type = TracedOp::APPEND;
          op.temp_write_seq = i->temp_write_seq;
        }
      }
      
      if (TracedOp::APPEND == op.type)
        op.append_seq = tracer_map.get_append_seq();
    } else {
      op.type = TracedOp::OVERLAY_WRITE;
    }
  } else if (end_pos > status.size) { // overlay write
    op.type = TracedOp::OVERLAY_WRITE;
  } else {
    op.type = TracedOp::PARTICAL_WRITE;
  }

  status.exist = true;
  if (op.type != TracedOp::PARTICAL_WRITE)
  {
    status.size = end_pos;
    dout(20) << "OpTracer " << this << " push " << op << " status = " << status << dendl;
    ops.push_back(op);
  }
  lock.Unlock();
  
  tracer_map.update_stat(op.type, len);
}

void OpTracer::commit_special_write(TracedOp& op, uint64_t end_pos)
{
  Mutex::Locker l(lock);
  assert(ops.empty() || ops.back() < op);

  status.exist = true;
  if (end_pos > status.size) {
    status.size = end_pos;
    op.type = TracedOp::OVERLAY_WRITE;
    ops.push_back(op);
    dout(20) << "OpTracer " << this << " push " << op << " status = " << status << dendl;
  }
}

void OpTracer::commit_resize(TracedOp& op, const Status& new_status)
{
  Mutex::Locker l(lock);
  assert(ops.empty() || ops.back() < op);

  status.exist = new_status.exist;
  if (new_status.size != status.size) {
    status.size = new_status.size;
    op.type = TracedOp::RESIZE;
    ops.push_back(op);
    dout(20) << "OpTracer " << this << " push " << " status = " << status << dendl;
  }
}

void OpTracer::finish(const TracedOp& op)
{
  Mutex::Locker l(lock);
  if (ops.size() && ops.front() == op) {
    assert(ops.front().type != TracedOp::PARTICAL_WRITE);
    dout(20) << "OpTracer " << this << " pop " << ops.front() << dendl;
    ops.pop_front();
  }
}

OpTracerMap::OpTracerMap(FileStore* _fs):
  fs(_fs),
  m_temp_write_min_length(g_conf->temp_write_min_length),
  m_append_min_length(g_conf->optimized_append_min_length),
  m_log_interval(g_conf->journal_optimize_log_interval),
  total_count(0), total_bytes(0),
  temp_write_count(0), temp_write_bytes(0),
  append_count(0), append_bytes(0),
  lock("OpTracerMap:lock")
{}

void OpTracerMap::update_stat(TracedOp::Type type, uint64_t len)
{
  if (!m_log_interval)
    return;
  
  Mutex::Locker l(lock);
  ++total_count;
  total_bytes += len;
  
  if (TracedOp::FULL_WRITE == type) {
    ++temp_write_count;
    temp_write_bytes += len;
  } else if (TracedOp::APPEND == type) {
    ++append_count;
    append_bytes += len;
  }
  
  if (0 == total_count % m_log_interval) {
    dout(1) << "total writes: " << total_count << ", " << total_bytes << " bytes" << dendl;
    if (temp_write_count)
      dout(1) << "temp writes: " << temp_write_count << ", " << temp_write_bytes << " bytes" << dendl;
    if (append_count)
      dout(1) << "optimized appends: " << append_count << ", " << append_bytes << " bytes" << dendl;
  }
}

OpTracerRef OpTracerMap::get(coll_t cid, const ghobject_t& oid)
{
  OpTracerRef tracer = tracers.lookup(oid);
  if (tracer)
    return tracer;
  
  OpTracer::Status status;
  struct stat buf;
  int retval = fs->lfn_stat(cid, oid, &buf);
  if (retval >= 0) {
    status.exist = true;
    status.size = (uint64_t)buf.st_size;
  } else if (retval != -ENOENT) {
    derr << "OpTracerMap:get stat " << cid << "/" << oid
         << " failed: " << cpp_strerror(retval) << dendl;
  }
  
  tracer = tracers.lookup_or_create(oid, status);
  dout(20) << "OpTracer[" << oid << "] = " << tracer.get() << ", status = " << status << dendl;
  return tracer;
}

OpTracerRef OpTracerMap::get(const ghobject_t& oid)
{
  OpTracerRef tracer = tracers.lookup(oid);
  if (tracer)
    return tracer;
  
  OpTracer::Status status;
  tracer = tracers.lookup_or_create(oid, status);
  dout(20) << "OpTracer[" << oid << "] = " << tracer.get() << dendl;
  return tracer;
}

AppendDescMap::AppendDescMap(FileStore* _fs):
  fs(_fs), dirty(true), 
  map_lock("AppendDescMap:map_lock"), garbage_lock("AppendDescMap:garbage_lock")
{
  ostringstream oss;
  oss << fs->basedir << "/temp/" << DESC_FILE;
  path = oss.str();
}

AppendDescMap::~AppendDescMap()
{
  assert(descs.empty());
  assert(garbage.empty());
}

void AppendDescMap::erase(uint64_t seq)
{
  if (fs->replaying) {
    Mutex::Locker l(map_lock);
    map<uint64_t, AppendDesc*>::iterator i = descs.find(seq);
    if (i != descs.end()) {
      dirty = true;
      dout(15) << "remove desc[" << seq << "] = " << *(i->second) << dendl;
      delete i->second;
      descs.erase(i);
    }
  } else {
    Mutex::Locker l(garbage_lock);
    garbage.push_back(seq);
  }
}

void AppendDescMap::encode(bufferlist& bl) const
{
  ENCODE_START(1, 1, bl);
  __u32 count = (__u32)(descs.size());
  ::encode(count, bl);
  for (map<uint64_t, AppendDesc*>::const_iterator i = descs.begin(); i != descs.end(); ++i) {
    ::encode(i->first, bl);
    ::encode(*(i->second), bl);
    dout(20) << "write desc[" << i->first << "] = " << *(i->second) << dendl;
  }
  ENCODE_FINISH(bl);
}

void AppendDescMap::decode(bufferlist::iterator& bp)
{
  __u32 count;
  uint64_t seq;
  AppendDesc* desc;
  
  DECODE_START(1, bp);
  ::decode(count, bp);
  while (count--) {
    ::decode(seq, bp);
    desc = new AppendDesc;
    ::decode(*desc, bp);
    descs[seq] = desc;
    dout(20) << "read desc[" << seq << "] = " << *desc << dendl;
  }
  DECODE_FINISH(bp);
}

int AppendDescMap::sync_add(const AppendDescList& adl)
{
  Mutex::Locker l(map_lock);
  for (AppendDescList::const_iterator i = adl.begin(); i != adl.end(); ++i) {
    dirty = true;
    dout(15) << " add desc[" << i->first << "] = " << *(i->second) << dendl;
    descs.insert(*i);
  }
  
  return sync_write();
}

int AppendDescMap::sync_remove()
{
  list<uint64_t> tmp;
  {
    Mutex::Locker l(garbage_lock);
    if (garbage.size())
      tmp.swap(garbage);
  }
  
  Mutex::Locker l(map_lock);
  if (tmp.size()) {
    for (list<uint64_t>::iterator li = tmp.begin(); li != tmp.end(); ++li) {
      map<uint64_t, AppendDesc*>::iterator mi = descs.find(*li);
      if (mi != descs.end()) {
        dirty = true;
        dout(15) << " remove desc[" << *li << "] = " << *(mi->second) << dendl;
        delete mi->second;
        descs.erase(mi);
      }
    }
  }
  
  return sync_write();
}

int AppendDescMap::create()
{ 
  dout(20) << "AppendDescMap:create " << path << dendl;
  
  int flags = O_WRONLY | O_CREAT | O_TRUNC | O_SYNC;
  int fd = TEMP_FAILURE_RETRY(::open(path.c_str(), flags, 0644));
  if (fd < 0) {
    int retval = -errno;
    derr << "AppendDescMap:create open " << path << " failed: " << cpp_strerror(retval) << dendl;
    return retval;
  }
  
  bufferlist bl;
  encode(bl);
  int retval = bl.write_fd(fd);
  if (retval < 0)
    derr << "AppendDescMap:create write " << path << " failed: " << cpp_strerror(retval) << dendl;

  VOID_TEMP_FAILURE_RETRY(::close(fd));
  return retval;
}

int AppendDescMap::sync_write()
{
  assert(map_lock.is_locked());
  if (!dirty)
    return 0;

  dout(20) << "AppendDescMap::sync_write()" << dendl;
  bufferlist bl;
  encode(bl);
  
  int flags = O_WRONLY | O_CREAT | O_TRUNC | O_SYNC;
  int fd = TEMP_FAILURE_RETRY(::openat(fs->temp_fd, TEMP_DESC_FILE, flags, 0644));
  if (fd < 0) {
    int retval = -errno;
    derr << "AppendDescMap:write open temp/" << TEMP_DESC_FILE 
         << " failed: " << cpp_strerror(retval) << dendl;
    return retval;
  }

  FDCache::FD pfd(fd);
  int retval = bl.write_fd(fd);
  if (retval < 0) {
    derr << "AppendDescMap:write write temp/" << TEMP_DESC_FILE
         << " failed: " << cpp_strerror(retval) << dendl;
    return retval;
  }
  
  retval = ::renameat(fs->temp_fd, TEMP_DESC_FILE, fs->temp_fd, DESC_FILE);
  if (retval < 0) {
    retval = -errno;
    derr << "AppendDescMap:write rename(" << TEMP_DESC_FILE << ", " << DESC_FILE 
         << ") in temp/ failed: " << cpp_strerror(retval) << dendl;
    return retval;
  }

  retval = ::fsync(fs->temp_fd);
  if (retval < 0) {
    retval = -errno;
    derr << "AppendDescMap:write fsync temp/ failed: " << cpp_strerror(retval) << dendl;
    return retval;
  }
  
  dirty = false;
  return 0;
}

int AppendDescMap::read()
{
  dout(20) << "AppendDescMap:read " << path << dendl;
  
  bufferlist bl;
  string error;
  int retval = bl.read_file(path.c_str(), &error);
  if (retval >= 0) {
    bufferlist::iterator i = bl.begin();
    try {
      decode(i);
    } catch (buffer::error& e) {
      derr << "AppendDescMap:read catch " << e.what() << dendl;
      return -EINVAL;
    }
  } else if (retval != -ENOENT) {
    derr << "AppendDescMap:read " << error << dendl;
    return retval;
  }
  
  dirty = false;
  return 0;
}

int AppendDescMap::rollback()
{
  dout(20) << "AppendDescMap:rollback " << dendl;

  bool changed(false);
  Mutex::Locker l(map_lock);
  for (map<uint64_t, AppendDesc*>::reverse_iterator ri = descs.rbegin(); ri != descs.rend(); ++ri) {
    AppendDesc* desc = ri->second;
    OpTracer::Status cur_status;

    FDRef fd;
    int r = fs->lfn_open(desc->cid, desc->oid, false, &fd);
    if (r >=0) {
      struct stat buf;
      ::fstat(**fd, &buf);
      cur_status.exist = true;
      cur_status.size = (uint64_t)buf.st_size;
    }
    
    if (!desc->old_status.exist && cur_status.exist) {
      // make sure the target object do not exist
      SequencerPosition spos(-1UL, 0, 0);
      r = fs->lfn_unlink(desc->cid, desc->oid, spos);
      derr << "rollback desc[" << ri->first << "] = " << *desc
           << " remove: " << cpp_strerror(r) << dendl;
      if (r >=0)
        changed = true;
    } else if (cur_status.size > desc->old_status.size) {
      r = ::ftruncate(**fd, desc->old_status.size);
      if (r < 0) {
        r = errno;
        derr << "rollback desc[" << ri->first << "] = " << *desc 
             << " ftruncate failed: " << cpp_strerror(r) << dendl;
      } else {
        changed = true;
        dout(10) << "rollback desc[" << ri->first << "] = " << *desc << dendl;
      }
    } else {
      dout(10) << "rollback ignore desc[" << ri->first << "]= " << *desc << dendl;
    }
    
    dirty = true;
    delete desc;
  }
  
  if (changed)
    fs->backend->syncfs();
  
  descs.clear();
  return sync_write();
}

int TempWrite::apply(FileStore* fs)
{
  return fs->_optimize_temp_write(*this);
}

int OptimizedAppend::apply(FileStore* fs)
{
  return fs->_optimize_append(*this);
}

void OptimizedAppend::add_desc(AppendDescList& adl)
{
  adl.push_back(make_pair(seq, new AppendDesc(cid, oid, old_status)));
}

void OptimizedOpList::set_pos(uint64_t seq)
{
  if (temp_write_count) {
    for (list<OptimizedOp*>::iterator i = ops.begin(); i != ops.end(); ++i) {
      TempWrite *p = dynamic_cast<TempWrite*>(*i);
      if (p)
        p->spos.seq = seq;
    }
  }
}

int OptimizedOpList::apply(FileStore* fs)
{
  int retval;
  list<OptimizedOp*>::iterator i;
  dout(10) << "OptimizedOpList:apply " << *this << dendl;
  
  if (append_count) {
    AppendDescList adl;
    for (i = ops.begin(); i != ops.end(); ++i)
      (*i)->add_desc(adl);
    retval = fs->append_desc_map.sync_add(adl);
    if (retval < 0)
      return retval;
  }
  
  for (i = ops.begin(); i != ops.end(); ++i) {
    retval = (*i)->apply(fs);
    if (retval < 0)
      return retval;
  }
  
  return 0;
}

ostream& operator<<(ostream& os, const OptimizedOpList& ops)
{
  if (ops.temp_write_count)
    os << " " << ops.temp_write_count << " temp-writes";
  if (ops.append_count)
    os << " " << ops.append_count << " appends";
  if (ops.bytes)
    os << " " << ops.bytes << " bytes";
  
  return os;
}

OptimizedOpList* FileStore::_optimize_parse_transactions(
  list<Transaction*>& tls,
  list<Transaction*>& new_tls,
  uint64_t& tls_num,
  set<OpTracerRef>& tracers)
{
  tls_num = op_tracer_map.get_tls_num();
  
  TracedOp top(tls_num);
  OptimizedOpList* op_list = new OptimizedOpList;
  list<Transaction*>::iterator itr;
  
  for (itr = tls.begin(); itr != tls.end(); ++itr, ++top.trans_num) {
    Transaction::iterator i = (*itr)->begin();
    Transaction *new_t = new Transaction;
    if (i.get_replica())
      new_t->set_replica();
    
    top.op_num = 0;
    while (i.have_op()) {
      __u32 op = i.decode_op();
      
      switch (op) {
      case Transaction::OP_NOP:
        new_t->nop();
        break;
      case Transaction::OP_TOUCH:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          new_t->touch(cid, oid);
        }
        break;
      case Transaction::OP_WRITE:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          uint64_t off = i.decode_length();
          uint64_t len = i.decode_length();

          OpTracerRef tracer = op_tracer_map.get(cid, oid);
          tracers.insert(tracer);
          OpTracer::Status old_status = tracer->get_status();
          tracer->commit_write(op_tracer_map, top, off, len);
          if (TracedOp::FULL_WRITE == top.type) {
            dout(20) << __func__ << " FULL_WRITE len = " << len << dendl;
            op_list->temp_write(top.temp_write_seq, len, i, top.trans_num, top.op_num);
            new_t->optimize_rename(top.temp_write_seq, len, cid, oid);
          } else if (TracedOp::APPEND == top.type) {
            dout(20) << __func__ << " APPEND off = " << off << " len = " << len << dendl;
            op_list->append(top.append_seq, off, len, i, cid, oid, old_status, top.temp_write_seq);
            new_t->optimize_finish_append(top.append_seq, oid);
          } else {
            bufferlist bl;
            i.decode_bl(bl);
            new_t->write(cid, oid, off, len, bl);
          }
        }
        break;
      case Transaction::OP_ZERO:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          uint64_t off = i.decode_length();
          uint64_t len = i.decode_length();
          OpTracerRef tracer = op_tracer_map.get(cid, oid);
          tracers.insert(tracer);
          tracer->commit_special_write(top, off + len);
          new_t->zero(cid, oid, off, len);
        }
        break;
      case Transaction::OP_TRIMCACHE: // deprecated, no-op
        i.decode_cid();
        i.decode_oid();
        i.decode_length();
        i.decode_length();
        dout(5) << __func__ << " OP_TRIMCACHE" << dendl;
        break;
      case Transaction::OP_TRUNCATE:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          uint64_t len = i.decode_length();
          OpTracerRef tracer = op_tracer_map.get(oid);
          tracers.insert(tracer);
          tracer->commit_resize(top, len);
          new_t->truncate(cid, oid, len);
        }
        break;
      case Transaction::OP_REMOVE:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          OpTracerRef tracer = op_tracer_map.get(oid);
          tracers.insert(tracer);
          tracer->commit_resize(top, 0);
          new_t->remove(cid, oid);
        }
        break;
      case Transaction::OP_SETATTR:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          string name = i.decode_attrname();
          bufferlist bl;
          i.decode_bl(bl);
          new_t->setattr(cid, oid, name, bl);
        }
        break;
      case Transaction::OP_SETATTRS:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          map<string, bufferptr> attr_set;
          i.decode_attrset(attr_set);
          new_t->setattrs(cid, oid, attr_set);
        }
        break;
      case Transaction::OP_RMATTR:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          string name = i.decode_attrname();
          new_t->rmattr(cid, oid, name);
        }
        break;
      case Transaction::OP_RMATTRS:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          new_t->rmattrs(cid, oid);
        }
        break;
      case Transaction::OP_CLONE:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          ghobject_t noid = i.decode_oid();
          struct stat buf;
          int retval = lfn_stat(cid, oid, &buf);
          if (retval >= 0) {
            OpTracerRef tracer = op_tracer_map.get(noid);
            tracers.insert(tracer);
            tracer->commit_resize(top, (uint64_t)buf.st_size);
          } else {
            derr << __func__ << " OP_CLONE stat " << cid << "/" << oid
                << " failed: " << cpp_strerror(retval) << dendl;
          }
          new_t->clone(cid, oid, noid);
        }
        break;
      case Transaction::OP_CLONERANGE: // changed to OP_CLONERANGE2
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          ghobject_t noid = i.decode_oid();
          uint64_t off = i.decode_length();
          uint64_t len = i.decode_length();
          OpTracerRef tracer = op_tracer_map.get(cid, noid);
          tracers.insert(tracer);
          tracer->commit_special_write(top, off + len);
          new_t->clone_range(cid, oid, noid, off, len, off);
        }
        break;
      case Transaction::OP_CLONERANGE2:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          ghobject_t noid = i.decode_oid();
          uint64_t srcoff = i.decode_length();
          uint64_t len = i.decode_length();
          uint64_t dstoff = i.decode_length();
          OpTracerRef tracer = op_tracer_map.get(cid, noid);
          tracers.insert(tracer);
          tracer->commit_special_write(top, dstoff + len);
          new_t->clone_range(cid, oid, noid, srcoff, len, dstoff);
        }
        break;
      case Transaction::OP_MKCOLL:
        {
          coll_t cid = i.decode_cid();
          new_t->create_collection(cid);
        }
        break;
      case Transaction::OP_COLL_HINT:
        {
          coll_t cid = i.decode_cid();
          uint32_t type = i.decode_u32();
          bufferlist hint;
          i.decode_bl(hint);
          new_t->collection_hint(cid, type, hint);
        }
        break;
      case Transaction::OP_RMCOLL:
        {
          coll_t cid = i.decode_cid();
          new_t->remove_collection(cid);
        }
        break;
      case Transaction::OP_COLL_ADD:
        {
          coll_t ncid = i.decode_cid();
          coll_t ocid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          new_t->collection_add(ncid, ocid, oid);
          dout(5) << __func__ << " OP_COLL_ADD" << dendl;
        }
        break;
      case Transaction::OP_COLL_REMOVE:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          new_t->remove(cid, oid);
          dout(5) << __func__ << " OP_COLL_REMOVE" << dendl;
        }
        break;
      // case Transaction::OP_COLL_MOVE: // deprecated and buggy
      case Transaction::OP_COLL_MOVE_RENAME:
        {
          coll_t ocid = i.decode_cid();
          ghobject_t ooid = i.decode_oid();
          coll_t ncid = i.decode_cid();
          ghobject_t noid = i.decode_oid();

          OpTracerRef tracer = op_tracer_map.get(ooid);
          tracers.insert(tracer);
          tracer->commit_resize(top, 0);
          
          struct stat buf;
          int retval = lfn_stat(ocid, ooid, &buf);
          if (retval >= 0) {
            tracer = op_tracer_map.get(noid);
            tracers.insert(tracer);
            tracer->commit_resize(top, (uint64_t)buf.st_size);
          } else {
            derr << __func__ << " OP_COLL_MOVE_RENAME stat " << ocid << "/" << ooid
                 << " failed: " << cpp_strerror(retval) << dendl;
          }
          new_t->collection_move_rename(ocid, ooid, ncid, noid);
        }
        break;
      case Transaction::OP_COLL_SETATTR:
        {
          coll_t cid = i.decode_cid();
          string name = i.decode_attrname();
          bufferlist bl;
          i.decode_bl(bl);
          new_t->collection_setattr(cid, name, bl);
        }
        break;
      case Transaction::OP_COLL_RMATTR:
        {
          coll_t cid = i.decode_cid();
          string name = i.decode_attrname();
          new_t->collection_rmattr(cid, name);
        }
        break;
      // case Transaction::OP_COLL_SETATTRS: // not implemented, so how?
      case Transaction::OP_STARTSYNC:
        new_t->start_sync();
        break;
      case Transaction::OP_COLL_RENAME:
        {
          coll_t cid = i.decode_cid();
          coll_t ncid = i.decode_cid();
          new_t->collection_rename(cid, ncid);
          dout(5) << __func__ << " OP_COLL_RENAME" << dendl;
        }
        break;
      case Transaction::OP_OMAP_CLEAR:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          new_t->omap_clear(cid, oid);
        }
        break;
      case Transaction::OP_OMAP_SETKEYS:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          map<string, bufferlist> attr_set;
          i.decode_attrset(attr_set);
          new_t->omap_setkeys(cid, oid, attr_set);
        }
        break;
      case Transaction::OP_OMAP_RMKEYS:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          set<string> keys;
          i.decode_keyset(keys);
          new_t->omap_rmkeys(cid, oid, keys);
        }
        break;
      case Transaction::OP_OMAP_RMKEYRANGE:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          string first = i.decode_key();
          string last = i.decode_key();
          new_t->omap_rmkeyrange(cid, oid, first, last);
        }
        break;
      case Transaction::OP_OMAP_SETHEADER:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          bufferlist bl;
          i.decode_bl(bl);
          new_t->omap_setheader(cid, oid, bl);
        }
        break;
      // case Transaction::OP_SPLIT_COLLECTION: // deprecated?
      case Transaction::OP_SPLIT_COLLECTION2:
        {
          coll_t cid = i.decode_cid();
          uint32_t bits = i.decode_u32();
          uint32_t rem = i.decode_u32();
          coll_t dest = i.decode_cid();
          new_t->split_collection(cid, bits, rem, dest);
          dout(5) << __func__ << " OP_SPLIT_COLLECTION2" << dendl;
        }
        break;
      case Transaction::OP_SETALLOCHINT:
        {
          coll_t cid = i.decode_cid();
          ghobject_t oid = i.decode_oid();
          uint64_t expected_object_size = i.decode_length();
          uint64_t expected_write_size = i.decode_length();
          new_t->set_alloc_hint(cid, oid, expected_object_size, expected_write_size);
        }
        break;
      default:
        derr << "bad op " << op << dendl;
        assert(0);
      }
      
      ++top.op_num;
    }
    
    new_tls.push_back(new_t);
  }
  
  if (op_list->get_bytes()) {
    list<Transaction*>::iterator new_itr = new_tls.begin();
    for (itr = tls.begin(); itr != tls.end(); ++itr, ++new_itr)
    {
      (*new_itr)->register_on_applied((*itr)->get_on_applied());
      (*new_itr)->register_on_commit((*itr)->get_on_commit());
      (*new_itr)->register_on_applied_sync((*itr)->get_on_applied_sync());
      (*new_itr)->register_on_applied(new C_DeleteTransaction(*new_itr)); // is OK ?
    }
    
    dout(10) << __func__ << " tls_num " << tls_num << " optimized:" << *op_list << dendl;
    return op_list;
  }
  
  for (itr = new_tls.begin(); itr != new_tls.end(); ++itr)
    delete *itr;
  new_tls.clear();
  
  delete op_list;
  return NULL;
}

int FileStore::_optimize_write_fd(int fd, const bufferlist& bl, uint64_t off, uint64_t len)
{
  int retval = ::ftruncate(fd, off + len);
  if (retval < 0) {
    retval = -errno;
    derr << __func__ << " truncate to " << (off + len) << " failed " << dendl;
  }
  
  if (off) {
    uint64_t actual = ::lseek64(fd, off, SEEK_SET);
    if (actual < off) {
      retval = -errno;
      derr << __func__ << " seek " << off << " failed " << dendl;
      return retval;
    }

    if (actual != off) {
      derr << __func__ << " seek " << off << " gave bad offset " << actual << dendl;
      return -EIO;
    }
  }
  
  retval = bl.write_fd(fd);
  if (retval >= 0) {
    if (m_filestore_sloppy_crc) {
      int rc = backend->_crc_update_write(fd, off, len, bl);
      assert(rc >= 0);
    }
    
    return 0;
  }
  
  derr << __func__ << " write failed " << dendl;
  return retval;
}

int FileStore::_optimize_temp_open(uint32_t seq, bool create, FDRef* outfd, bool dio)
{
  if (0/*!replaying && !create*/) {
    *outfd = temp_fd_cache.lookup(seq);
    if (*outfd) {
      return 0;
    }
  }
  
  char name[20];
  sprintf(name, "%x", seq);
  dout(15) << __func__ << " seq = " << seq << " name = " << name
       << " create = " << (create ? "true" : "false") << dendl;
  
  int flags = O_RDWR;
  if (create)
    flags |= O_CREAT;
  if (dio)
    flags |= O_DIRECT;
  if (TEMP_WRITE_OSYNC == m_temp_write_sync_by)
    flags |= O_SYNC;
  
  int fd(-1);
  for (int i = 0; i < 5; i++) { // retry when meet ENOSPC error
    fd = TEMP_FAILURE_RETRY(::openat(temp_fd, name, flags, 0644));
    if (fd >= 0 || errno != ENOSPC) {
      if (i > 0) {
        dout(0) << "create temp/" << name << " retried " 
             << i << " time(s) for ENOSPC" << dendl;
      }
      break;
    }
    utime_t(0, 1000000).sleep();
  }
  if (fd < 0)
    return -errno;
  
  FDCache::FD* pfd = new FDCache::FD(fd);
  if (0/*!replaying*/) {
    bool existed;
    *outfd = temp_fd_cache.add(seq, pfd, &existed);
    if (existed)
      delete pfd;
  } else {
    *outfd = FDRef(pfd);
  }
  
  return 0;
}

int FileStore::_optimize_temp_write(TempWrite& op)
{
  assert(op.spos.seq);
  dout(15) << __func__ << " " << op << dendl;
  
  // open
  FDRef fd;
  bool dio = m_temp_write_dio && op.page_aligned;
  int retval = _optimize_temp_open(op.seq, true, &fd, dio);
  if (retval < 0) {
    derr << __func__ << " seq = " << op.seq << " open failed: " << cpp_strerror(retval) << dendl;
    return retval;
  }
  
  // write
  if (dio)
    op.bl.rebuild_page_aligned();
  retval = _optimize_write_fd(**fd, op.bl, 0, op.len);
  if (retval < 0) {
    derr << __func__ << " seq = " << op.seq << " write failed: " << cpp_strerror(retval) << dendl;
    return retval;
  }

  // set replay-guard
  bool in_progress(false);
  bufferlist v(40);
  ::encode(op.spos, v);
  ::encode(in_progress, v);
  retval = chain_fsetxattr(**fd, REPLAY_GUARD_XATTR, v.c_str(), v.length());
  if (retval < 0) {
    derr << __func__ << " seq = " << op.seq << " setxattr " << REPLAY_GUARD_XATTR
         << " failed: " << cpp_strerror(retval) << dendl;
    return retval;
  }
  
  // sync file
  if (TEMP_WRITE_FSYNC == m_temp_write_sync_by) {
    retval = ::fdatasync(**fd);
    if (retval < 0) {
      retval = -errno;
      derr << __func__ << " seq = " << op.seq 
           << " fdatasync failed: " << cpp_strerror(retval) << dendl;
      return retval;
    }
  }
  
  return 0;
}

int FileStore::_optimize_rename(uint32_t seq, coll_t& cid, const ghobject_t& oid)
{
  char name[20];
  sprintf(name, "%x", seq);
  dout(15) << __func__ << " from temp/" << name << " to " << cid << "/" << oid << dendl;

  // check if the temp object exist
  if (replaying) { 
    string temp_path = basedir;
    temp_path += "/temp/";
    temp_path += name;

    struct stat buf;
    int retval = ::stat(temp_path.c_str(), &buf);
    if (retval < 0) {
      if (ENOENT == errno) {
        dout(10) << __func__ << " temp/" << name << " already renamed" << dendl;
        return 0;
      }

      retval = -errno;
      derr << __func__ << " stat " << temp_path << " failed: " << cpp_strerror(retval) << dendl;
      return retval;
    }
  }
  
  // get path of target object
  Index index; 
  int retval = get_index(cid, &index);
  if (retval < 0) {
    derr << __func__ << " get index for " << cid 
         << " failed: " << cpp_strerror(retval) << dendl;
    return retval;
  }
  
  bool create(false);
  IndexedPath indexedPath;
  RWLock::WLocker l((index.index)->access_lock);
  retval = lfn_find(oid, index, &indexedPath);
  if (-ENOENT == retval) {
    create = true;
  } else if (retval < 0) {
    derr << __func__ << " find " << oid << " in index"
         << " failed: " << cpp_strerror(retval) << dendl;
    return retval;
  }
  
  string path = indexedPath->path();
  
  // target object exists, copy its attrs to temp object
  if (!create) {
    // open temp object
    FDRef src_fd;
    retval = _optimize_temp_open(seq, false, &src_fd);
    if (retval < 0) {
      derr << __func__ << " open temp/" << name
           << " failed: " << cpp_strerror(retval) << dendl;
    } else {
      // open target object
      int dest_fd = TEMP_FAILURE_RETRY(::open(path.c_str(), O_RDONLY));
      if (dest_fd < 0) {
        retval = -errno;
        derr << __func__ << " open " << path
             << " failed: " << cpp_strerror(retval) << dendl;
      } else {
        // get attrs from target object
        map<string, bufferptr> attr_set;
        retval = _fgetattrs(dest_fd, attr_set);
        if (retval < 0) {
          derr << __func__ << " get attrs of " << path
               << " failed: " << cpp_strerror(retval) << dendl;
        } else  if (attr_set.size()) {
          // do NOT override the replay-guard in temp object
          attr_set.erase(REPLAY_GUARD_XATTR);
          // set attrs to temp object
          retval = _fsetattrs(**src_fd, attr_set); 
          if (retval < 0) {
            derr << __func__ << " set attrs for temp/" << name
                 << " failed: " << cpp_strerror(retval) << dendl; 
          } /* else { // make sure the attrs are on disk(for replay guard)
            retval = ::fsync(**src_fd);
            if (retval < 0) {
              retval = -errno;
              derr << __func__ << " fsync temp/" << name 
                   << " failed: " << cpp_strerror(retval) << dendl;
            }
          } */
        }
        
        VOID_TEMP_FAILURE_RETRY(::close(dest_fd));
      }
    }
  }
  
  // rename
  retval = ::renameat(temp_fd, name, AT_FDCWD, path.c_str());
  if (retval < 0)
  {
    retval = -errno;
    derr << __func__ << " rename(temp/" << name << ", " << path 
         << ") failed: " << cpp_strerror(retval) << dendl;
    return retval;
  }
  
  if (!replaying) { // clear cache
    if (g_conf->filestore_wbthrottle_enable)
      wbthrottle.clear_object(oid);

    fdcache.clear(oid);
    temp_fd_cache.clear(seq);
  }
  
  /*if (create) { // add the new object to index    
    retval = index->created(oid, path.c_str());
    if (retval < 0) {
      derr << __func__ << " create " << path << " in index"
           << " failed: " << cpp_strerror(retval) << dendl;
    }
  }*/
  
  return retval;
}

void FileStore::_optimize_clear_temp_objects()
{
  string path(basedir);
  path += "/temp";
  DIR *dir = ::opendir(path.c_str());
  if (!dir) {
    int err = errno;
    derr << __func__ << " open " << path << "/ failed: " << cpp_strerror(err) << dendl;
    return;
  }
  
  int count(0);
  while (true) {
    errno = 0;
    struct dirent *entry = ::readdir(dir);
    if (entry) {
      if (!strcmp(entry->d_name, ".") ||
          !strcmp(entry->d_name, "..") ||
          !strcmp(entry->d_name, DESC_FILE))
        continue;
      
      if (::unlinkat(temp_fd, entry->d_name, 0) < 0) {
        int err = errno;
        derr << __func__ << " unlink temp/" << entry->d_name
             << " failed: " << cpp_strerror(err) << dendl;
        continue;
      }
      
      dout(5) << __func__ << " delete " << entry->d_name << dendl;
      ++count;
    } else {
      int err = errno;
      if (err) {
        derr << __func__ << " read " << path << "/ failed: " << cpp_strerror(err) << dendl;
      }
      break;
    }
  }
  
  VOID_TEMP_FAILURE_RETRY(::closedir(dir));
  if (count) {
    dout(1) << __func__ << " delete " << count << " temp objects" << dendl;
  }
}

int FileStore::_optimize_append(OptimizedAppend& op)
{
  FDRef fd;
  dout(15) << __func__ << " " << op << dendl;
  
  if (op.temp_write_seq) {
    // open
    int retval = _optimize_temp_open(op.temp_write_seq, false, &fd);
    if (retval >= 0) {
      dout(10) << __func__ << " append to temp " << op.temp_write_seq << dendl;
      
      // write
      retval = _optimize_write_fd(**fd, op.bl, op.off, op.len);
      if (retval < 0) {
        derr << __func__ << " append to temp" << op.temp_write_seq
             << " failed: " << cpp_strerror(retval) << dendl;
      }
      
      return retval;
    } else if (retval != -ENOENT) {
      derr << __func__ << " open temp " << op.temp_write_seq
           << " failed: " << cpp_strerror(retval) << dendl;
    }
  }
  
  dout(10) << __func__ << " append to " << op.cid << "/" << op.oid << dendl;
  
  bool create = !op.old_status.exist;
  int retval = lfn_open(op.cid, op.oid, create, &fd);
  if (retval < 0) {
    derr << __func__ << " open " << op.cid << "/" << op.oid
         << " failed: " << cpp_strerror(retval) << dendl;
    return retval;
  }

  retval = _optimize_write_fd(**fd, op.bl, op.off, op.len);
  if (retval < 0) {
    derr << __func__ << " append to " << op.cid << "/" << op.oid
         << " failed: " << cpp_strerror(retval) << dendl;
  }
  
  return retval;
}

void FileStore::_optimize_syncfs()
{
  if (TEMP_WRITE_SYNCFS == m_temp_write_sync_by)
    backend->syncfs();
  else
    ::fsync(temp_fd);
}

