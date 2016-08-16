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


#ifndef CEPH_JOURNAL_H
#define CEPH_JOURNAL_H

#include <errno.h>

#include "include/buffer.h"
#include "include/Context.h"
#include "common/Finisher.h"
#include "common/TrackedOp.h"
#include "QueuedItem.h"

class PerfCounters;

class Journal {
protected:
  uuid_d fsid;
  Finisher *finisher;
public:
  PerfCounters *logger;
protected:
  Cond *do_sync_cond;
  bool wait_on_full;

public:
  Journal(uuid_d f, Finisher *fin, Cond *c=0) :
    fsid(f), finisher(fin), logger(NULL),
    do_sync_cond(c),
    wait_on_full(false) { }
  virtual ~Journal() { }

  virtual int check() = 0;   ///< check if journal appears valid
  virtual int create() = 0;  ///< create a fresh journal
  virtual int open(uint64_t fs_op_seq) = 0;  ///< open an existing journal
  virtual void close() = 0;  ///< close an open journal

  virtual void flush() = 0;
  virtual void throttle() = 0;

  virtual int dump(ostream& out) { return -EOPNOTSUPP; }

  void set_wait_on_full(bool b) { wait_on_full = b; }

  // writes
  virtual bool is_writeable() = 0;
  virtual int make_writeable() = 0;
  virtual void submit_entry(uint64_t seq, bufferlist& e, int alignment,
			    Context *oncommit,
			    TrackedOpRef osd_op = TrackedOpRef(),
			    void *extra = NULL) = 0; // journal-tuning
  virtual void submit_entry(QueuedItem* item) = 0;
  virtual void commit_start(uint64_t seq) = 0;
  virtual void committed_thru(uint64_t seq) = 0;

  /// Read next journal entry - asserts on invalid journal
  virtual bool read_entry(
    bufferlist &bl, ///< [out] payload on successful read
    uint64_t &seq   ///< [in,out] sequence number on last successful read
    ) = 0; ///< @return true on successful read, false on journal end

  virtual bool should_commit_now() = 0;

	
	// reads/recovery
	
	// kj 20150923 start
	
	// 对日志进行预读，解析出transactions
	// 调用update_stash_info对transactions进行在解析成单独的transaction，并完成map创建或者插入
	// 需要注意  off64_t pos = read_pos; 日志内部指针起始值的保存或者设置
	virtual void pre_read_journal_entry(
			uint64_t pre_op_seq 	///< [in,out] sequence number on last successful read
			) = 0; 

	// kj 20150923 end

  // reads/recovery
  
};

#endif
