#ifndef CEPH_JOURNAL_OPTIMIZE_H
#define CEPH_JOURNAL_OPTIMIZE_H

#include "common/sharedptr_registry.hpp"
#include "ObjectStore.h"
#include "SequencerPosition.h"

class OpTracerMap;

struct TracedOp {
  enum Type {
    UNKNOWN = 0,
    FULL_WRITE, // off = 0 && len >= size && optimized
    APPEND, // off = size && optimized
    OVERLAY_WRITE, // off + len > size
    PARTICAL_WRITE, // other writes
    RESIZE, // truncate, remove, clone(target), etc.
  } type;
  
  const char *get_type() const {
    switch (type) {
    case UNKNOWN: return "unknown";
    case FULL_WRITE: return "full-write";
    case APPEND: return "append";
    case OVERLAY_WRITE: return "overlay-write";
    case PARTICAL_WRITE: return "partical-write";
    case RESIZE: return "resize";
    default: return "invalid type";
    }
  }
  
  uint64_t tls_num; // serial number of tls
  uint32_t trans_num; // serial number of transaction in tls
  uint32_t op_num; // serial number of operation in transaction
  
  uint32_t temp_write_seq; // serial number of temp-write
  uint64_t append_seq; // serial number of optimized-append
  
  TracedOp(uint64_t _tls_num = 0, uint64_t _trans_num = 0): 
    type(UNKNOWN),
    tls_num(_tls_num), trans_num(_trans_num), op_num(0),
    temp_write_seq(0), append_seq(0)
  {}
};

ostream& operator<<(ostream& os, const TracedOp& op);
WRITE_EQ_OPERATORS_3(TracedOp, tls_num, trans_num, op_num)
WRITE_CMP_OPERATORS_3(TracedOp, tls_num, trans_num, op_num)

class OpTracer {
public:
  struct Status {
    bool exist;
    uint64_t size;
    
    Status(bool _exist = false, uint64_t _size = 0): exist(_exist), size(_size) {}
  };
  
  OpTracer(const Status& _status): status(_status), lock("OpTracer:lock") {}
  ~OpTracer();
  
  const Status& get_status() const { return status; }
  
  // commit write, return true if the op can be optimized
  void commit_write(OpTracerMap& tracer_map, TracedOp& op, uint64_t off, uint64_t len);

  // commit zero/clonerange/clonerange2
  // which may change obj's size, but can never be optimized
  void commit_special_write(TracedOp& op, uint64_t end_pos);
  
  // commit touch/truncate/delete/clone, which may change obj's status
  void commit_resize(TracedOp& op, const Status& _status);
  
  // finish an operation
  void finish(const TracedOp& op);
  
private:
  Status status; // current status
  list<TracedOp> ops; // list of "change-size" operations
  Mutex lock;
};

inline ostream& operator<<(ostream& os, const OpTracer::Status& status)
{
  return os << "(" << (status.exist ? "true" : "false")
            << ", " << status.size << ")";
}

typedef ceph::shared_ptr<OpTracer> OpTracerRef;

class FileStore;

class OpTracerMap {
private:
  FileStore* fs;
  SharedPtrRegistry<ghobject_t, OpTracer> tracers;
  
  ceph::atomic64_t last_tls_num;
  ceph::atomic64_t last_temp_write_seq;
  ceph::atomic64_t last_append_seq;

  uint64_t m_temp_write_min_length;
  uint64_t m_append_min_length;
  uint64_t m_log_interval;

  uint64_t total_count, total_bytes;
  uint64_t temp_write_count, temp_write_bytes;
  uint64_t append_count, append_bytes;
  Mutex lock;
  
public:
  OpTracerMap(FileStore* _fs);
  
  ~OpTracerMap() {
    assert(tracers.empty());
  }
  
  uint64_t get_tls_num() { return last_tls_num.inc(); }
  uint64_t get_temp_write_seq() { return last_temp_write_seq.inc(); }
  uint64_t get_append_seq() { return last_append_seq.inc(); }

  bool temp_write_ok(uint64_t len) const 
    { return m_temp_write_min_length && len >= m_temp_write_min_length; }
  bool append_ok(uint64_t len) const 
    { return m_append_min_length && len >= m_append_min_length; }

  void update_stat(TracedOp::Type type, uint64_t len);

  OpTracerRef get(coll_t cid, const ghobject_t& oid);
  OpTracerRef get(const ghobject_t& oid);
};

struct AppendDesc {
  coll_t cid;
  ghobject_t oid;
  OpTracer::Status old_status;

  AppendDesc(coll_t _cid, const ghobject_t _oid, const OpTracer::Status& _status)
  : cid(_cid), oid(_oid), old_status(_status) {}
  
  AppendDesc() {}
  
  void encode(bufferlist& bl) const {
    ::encode(cid, bl);
    ::encode(oid, bl);
    ::encode(old_status.exist, bl);
    ::encode(old_status.size, bl);
  }
  
  void decode(bufferlist::iterator& bp) {
    ::decode(cid, bp);
    ::decode(oid, bp);
    ::decode(old_status.exist, bp);
    ::decode(old_status.size, bp);
  }
  
private:
  AppendDesc(const AppendDesc&);
  AppendDesc& operator=(const AppendDesc&);
};

inline ostream& operator<<(ostream& os, const AppendDesc& desc)
{
  return os << "(" << desc.cid << "/" << desc.oid << ": " << desc.old_status << ")";
}

typedef list<pair<uint64_t, AppendDesc*> > AppendDescList;

class AppendDescMap {
public:
  AppendDescMap(FileStore* _fs);
  ~AppendDescMap();
  
  void erase(uint64_t seq);
  
  int sync_add(const AppendDescList& adl);
  int sync_remove();
  
  int create();
  int read();
  int rollback();
  
private:
  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& bp);
  
  int sync_write();
  
  FileStore* fs;
  string path;
  map<uint64_t, AppendDesc*> descs; // seq -> AppendDesc
  bool dirty;
  Mutex map_lock;
  
  list<uint64_t> garbage;
  Mutex garbage_lock;
};

typedef ObjectStore::Transaction::iterator TransItr;

struct OptimizedOp {
  uint64_t len;
  bufferlist bl;
  bool page_aligned;
  
  OptimizedOp(uint64_t _len, TransItr& itr): len(_len), page_aligned(false) {
    itr.decode_bl(bl);
    assert(len == bl.length());
  }
  
  virtual ~OptimizedOp() {}
  
  virtual int apply(FileStore* fs) = 0;
  
  virtual void add_desc(AppendDescList& adl) {}
};

struct TempWrite: public OptimizedOp {
  uint32_t seq;
  SequencerPosition spos;

  TempWrite(uint32_t _seq, uint64_t _len, TransItr& itr, uint32_t trans_num, uint32_t op_num): 
    OptimizedOp(_len, itr), seq(_seq), spos(0, trans_num, op_num) {
    if (CEPH_PAGE_ALIGNED(len))
      page_aligned = true;
  }

  int apply(FileStore* fs);
};

inline ostream& operator<<(ostream& os, const TempWrite& op)
{
  return os << "TempWrite[seq = " << op.seq << ", len = " << op.len << ", spos = " << op.spos << "]";
}

struct OptimizedAppend: public OptimizedOp {
  uint64_t seq;
  uint64_t off;
  coll_t cid;
  ghobject_t oid;
  OpTracer::Status old_status;
  uint32_t temp_write_seq;
  
  OptimizedAppend(uint64_t _seq, uint64_t _off, uint64_t _len, TransItr& itr,
    coll_t _cid, const ghobject_t& _oid, const OpTracer::Status& _status, uint32_t _temp_write_seq): 
    OptimizedOp(_len, itr), seq(_seq), off(_off),
    cid(_cid), oid(_oid), old_status(_status),
    temp_write_seq(_temp_write_seq) {
    if (CEPH_PAGE_ALIGNED(off) && CEPH_PAGE_ALIGNED(len))
      page_aligned = true;
  }
  
  int apply(FileStore* fs);

  void add_desc(AppendDescList& adl);
};

inline ostream& operator<<(ostream& os, const OptimizedAppend& op)
{
  os << "OptimizedAppend[seq = " << op.seq << ", off = " << op.off << ", len = " << op.len;
  os << ", object = " << op.cid << "/" << op.oid << ", old_status = " << op.old_status;
  if (op.temp_write_seq)
    os << ", temp_write_seq = " << op.temp_write_seq;
  return os << "]";
}

class OptimizedOpList {
  list<OptimizedOp*> ops;
  uint64_t temp_write_count;
  uint64_t append_count;
  uint64_t bytes;
  
public:
  OptimizedOpList(): temp_write_count(0), append_count(0), bytes(0) {}
  
  ~OptimizedOpList() {
    for (list<OptimizedOp*>::iterator i = ops.begin(); i != ops.end(); ++i)
      delete *i;
  }
  
  uint64_t get_ops() const { return temp_write_count + append_count; }
  uint64_t get_bytes() const { return bytes; }
  
  void temp_write(uint32_t seq, uint64_t len, TransItr& itr, uint32_t trans_num, uint32_t op_num) {
    ops.push_back(new TempWrite(seq, len, itr, trans_num, op_num));
    ++temp_write_count;
    bytes += len;
  }
  
  void append(uint64_t seq, uint64_t off, uint64_t len, TransItr& itr,
    coll_t cid, const ghobject_t& oid, const OpTracer::Status& status, uint32_t temp_write_seq) {
    ops.push_back(new OptimizedAppend(seq, off, len, itr, cid, oid, status, temp_write_seq));
    ++append_count;
    bytes += len;
  }
  
  void set_pos(uint64_t seq);
  
  void splice(OptimizedOpList& another) {
    ops.splice(ops.end(), another.ops);
    temp_write_count += another.temp_write_count;
    append_count += another.append_count;
    bytes += another.bytes;
    
    another.temp_write_count = 0;
    another.append_count = 0;
    another.bytes = 0;
  }
  
  int apply(FileStore* fs);
  
  friend ostream& operator<<(ostream&, const OptimizedOpList&);
};

WRITE_CLASS_ENCODER(AppendDesc)

#endif

