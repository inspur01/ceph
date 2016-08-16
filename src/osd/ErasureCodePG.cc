#include "ErasureCodePG.h"
#include "DirectECProcess.h"

#define dout_subsys ceph_subsys_directec
#undef dout_prefix
#define dout_prefix (*_dout << "ecpg[" << info.pgid << "] ")

ErasureCodePG::ErasureCodePG(OSDService* o, OSDMapRef curmap, 
    const PGPool& _pool, spg_t p, 
    const hobject_t& oid, const hobject_t& ioid)
:ReplicatedPG(o, curmap, _pool, p, oid, ioid)
{
  uint64_t alignment = pool.info.required_alignment();
  decp = new DirectECProcess(alignment, pgbackend.get(), this);
}

ErasureCodePG::~ErasureCodePG()
{
  delete decp;
}
class C_OSD_RepopApplied : public Context {
  ErasureCodePGRef pg;
  boost::intrusive_ptr<ReplicatedPG::RepGather> repop;
public:
  C_OSD_RepopApplied(ErasureCodePG *pg, ReplicatedPG::RepGather *repop)
  : pg(pg), repop(repop) {}
  void finish(int) {
    pg->repop_all_applied(repop.get());
  }
};


class C_OSD_RepopCommit : public Context {
  ErasureCodePGRef pg;
  boost::intrusive_ptr<ReplicatedPG::RepGather> repop;
public:
  C_OSD_RepopCommit(ErasureCodePG *pg, ReplicatedPG::RepGather *repop)
    : pg(pg), repop(repop) {}
  void finish(int) {
    pg->repop_all_committed(repop.get());
  }
};

struct FillInExtent : public Context {
  ceph_le64 *r;
  FillInExtent(ceph_le64 *r) : r(r) {}
  void finish(int _r) {
    if (_r >= 0) {
      *r = _r;
    }
  }
};

void ErasureCodePG::execute_ctx(OpContext* ctx)
{
  dout(10) << __func__ << " " << ctx << dendl;
  ctx->reset_obs(ctx->obc);
  OpRequestRef op = ctx->op;
  MOSDOp* m = static_cast<MOSDOp *>(op->get_req());
  ObjectContextRef obc = ctx->obc;
  const hobject_t& soid = obc->obs.oi.soid;
  map<hobject_t, ObjectContextRef>& src_obc = ctx->src_obc;

  // this method must be idempotent since we may call it several times
  // before we finally apply the resulting transaction.
  delete ctx->op_t;
  ctx->op_t = NULL;

  bool may_write = op->may_write() || op->may_cache();
  if (may_write) {
    op->mark_started();

    // snap
    if (!(m->get_flags() & CEPH_OSD_FLAG_ENFORCE_SNAPC) &&
        pool.info.is_pool_snaps_mode()) {
      // use pool's snapc
      ctx->snapc = pool.snapc;
    } else {
      // client specified snapc
      ctx->snapc.seq = m->get_snap_seq();
      ctx->snapc.snaps = m->get_snaps();
    }
    if ((m->get_flags() & CEPH_OSD_FLAG_ORDERSNAP) &&
        ctx->snapc.seq < obc->ssc->snapset.seq) {
      dout(5) << " ORDERSNAP flag set and snapc seq " << ctx->snapc.seq
        << " < snapset seq " << obc->ssc->snapset.seq
        << " on " << obc->obs.oi.soid << dendl;
      reply_ctx(ctx, -EOLDSNAPC);
      return;
    }

    ctx->mtime = m->get_mtime();
  } else {
    dout(10) << __func__ << " " << soid << " " << ctx->ops 
      << " ov " << obc->obs.oi.version << dendl;
  }

  if (!ctx->user_at_version)
    ctx->user_at_version = obc->obs.oi.user_version;
  dout(20) << __func__ << " user_at_version " << ctx->user_at_version << dendl;

  if (may_write) {
    int result = decp->check_osd_ops(ctx);
    if (result < 0) {
      reply_ctx(ctx, result);
      return;
    }

    // check for full
    if (ctx->delta_stats.num_bytes > 0 && 
        pool.info.get_flags() & pg_pool_t::FLAG_FULL) {
      reply_ctx(ctx, -ENOSPC);
      return;
    }

    // the ctx will be processed in DirectECProcess
    if (result > 0) {
      bool successful_write = ctx->num_write && op->may_write();
      // prepare the reply
      ctx->reply = new MOSDOpReply(m, 0, get_osdmap()->get_epoch(), 0, successful_write);
      ctx->reply->set_result(0);

      if (!ctx->num_write) { // all ops are error or nop
        MOSDOpReply* reply = ctx->reply;
        ctx->reply = NULL;

        log_op_stats(ctx);
        reply->set_reply_versions(eversion_t(), ctx->obs->oi.user_version);
        reply->add_flags(CEPH_OSD_FLAG_ACK | CEPH_OSD_FLAG_ONDISK);
        osd->send_message_osd_client(reply, m->get_connection());
        close_op_ctx(ctx, 0);
      } else {
        RepGather* repop = new_repop(ctx, obc, osd->get_tid());
        repop->src_obc.swap(src_obc);
        issue_repop_to_decp(repop);
        repop->put();
      }

      return;
    }

#if 0
    dout(0) << __func__ << " may write but there are ops not supportted by DirectECProcess!" << dendl;
    assert(false);
#endif

    ctx->op_t = pgbackend->get_transaction();
    ctx->current_osd_subop_num = 0;
    ctx->at_version = get_next_version();
    dout(10) << __func__ << soid << " " << ctx->ops 
      << " ov " << obc->obs.oi.version << " av " << ctx->at_version
      << " snapc " << ctx->snapc << " snapset " << obc->ssc->snapset << dendl;
  }

  if (op->may_read()) {
    dout(10) << " taking ondisk_read_lock" << dendl;
    obc->ondisk_read_lock();
  }
  for (map<hobject_t, ObjectContextRef>::iterator p = src_obc.begin(); p != src_obc.end(); ++p) {
    dout(10) << " taking ondisk_read_lock for src " << p->first << dendl;
    p->second->ondisk_read_lock();
  }

  int result = do_osd_ops(ctx, ctx->ops); // wumq

  if (op->may_read()) {
    dout(10) << " dropping ondisk_read_lock" << dendl;
    obc->ondisk_read_unlock();
  }
  for (map<hobject_t, ObjectContextRef>::iterator p = src_obc.begin(); p != src_obc.end(); ++p) {
    dout(10) << " dropping ondisk_read_lock for src " << p->first << dendl;
    p->second->ondisk_read_unlock();
  }

  // prepare the reply
  ctx->reply = new MOSDOpReply(m, 0, get_osdmap()->get_epoch(), 0, false);
  ctx->reply->set_result(result);
  if (ctx->pending_async_reads.empty()) {
    complete_read_ctx(result, ctx);
  } else {
    in_progress_async_reads.push_back(make_pair(op, ctx));
    ctx->start_async_reads(this);
  }
}

void ErasureCodePG::issue_repop_to_decp(RepGather* repop)
{
  OpContext* ctx = repop->ctx;
  const hobject_t& soid = ctx->obs->oi.soid;
  dout(10) << __func__ << " rep_tid " << repop->rep_tid << " o " << soid << dendl;

  repop->obc->ondisk_write_lock();
  if (ctx->clone_obc) // maybe useless
    ctx->clone_obc->ondisk_write_lock();

  bool unlock_snapset_obc(false);
  ObjectContextRef snapset_obc = ctx->snapset_obc;
  if (snapset_obc && snapset_obc->obs.oi.soid != soid) {
    snapset_obc->ondisk_write_lock();
    unlock_snapset_obc = true;
  }

  Context* on_all_commit = new C_OSD_RepopCommit(this, repop);
  Context* on_all_applied = new C_OSD_RepopApplied(this, repop);
  Context* onapplied_sync = new C_OSD_OndiskWriteUnlock(repop->obc, 
      ctx->clone_obc, unlock_snapset_obc ? snapset_obc : ObjectContextRef());

  decp->do_osd_ops(repop, onapplied_sync, on_all_applied, on_all_commit);
}

int ErasureCodePG::do_osd_ops(OpContext* ctx, vector<OSDOp>& ops) 
{
  assert(!ctx->ops.empty()); // wumq
  assert(!ctx->modify); // wumq

  if (!ctx->snapc.is_valid()) { // wumq
    dout(0) << "invalid snapc " << ctx->snapc << dendl;
    return -EINVAL;
  }

  ObjectState& obs = ctx->new_obs;
  object_info_t& oi = obs.oi;
  const hobject_t& soid = oi.soid;
  object_stat_sum_t& delta = ctx->delta_stats;
  int result = 0;
  bool first_read = true;

  dout(10) << __func__ << " " << soid << " " << ops << dendl;

  for (vector<OSDOp>::iterator p = ops.begin(); 
      p != ops.end(); ++p, ctx->current_osd_subop_num++) {
    ceph_osd_op& op = p->op;
    bufferlist::iterator bp = p->indata.begin();
    bufferlist& outdata = p->outdata;

    if (op.op & CEPH_OSD_OP_MODE_WR)
      derr << "do_osd_op(MODE_WR) " << *p << dendl;
    else 
      dout(10) << "do_osd_op " << *p << dendl;

    ObjectContextRef src_obc;
    if (ceph_osd_op_type_multi(op.op)) {
      MOSDOp* m = static_cast<MOSDOp *>(ctx->op->get_req());
      object_locator_t src_oloc;
      get_src_oloc(soid.oid, m->get_object_locator(), src_oloc);
      hobject_t src_oid(p->soid, src_oloc.key, soid.hash, info.pgid.pool(), src_oloc.nspace);
      src_obc = ctx->src_obc[src_oid];
      dout(10) << "src_oid " << src_oid << " obc " << src_obc << dendl;
      assert(src_obc);
    }

    switch (op.op) {
      case CEPH_OSD_OP_READ:
        ++ctx->num_read;
        {
          uint64_t size = oi.size;
          // are we beyond truncate_size?
          if ((oi.truncate_seq < op.extent.truncate_seq) &&
              (op.extent.offset + op.extent.length > op.extent.truncate_size))
            size = op.extent.truncate_size;

          if (op.extent.offset >= size) {
            op.extent.length = 0;
          } else {
            if (op.extent.offset + op.extent.length > size)
              op.extent.length = size - op.extent.offset;
            ctx->pending_async_reads.push_back(make_pair(
                  make_pair(op.extent.offset, op.extent.length),
                  make_pair(&outdata, new FillInExtent(&op.extent.length))));
          }

          if (first_read) {
            first_read = false;
            ctx->data_off = op.extent.offset;
          }
          delta.num_rd_kb += SHIFT_ROUND_UP(op.extent.length, 10);
          delta.num_rd++;
        }
        break;
      case CEPH_OSD_OP_STAT:
        if (obs.exists && !oi.is_whiteout()) {
          ::encode(oi.size, outdata);
          ::encode(oi.mtime, outdata);
          dout(10) << "stat oi has " << oi.size << " " << oi.mtime << dendl;
        } else {
          result = -ENOENT;
          dout(10) << "stat oi ENOENT" << dendl;
        }
        delta.num_rd++;
        break;
      case CEPH_OSD_OP_GETXATTR:
        ++ctx->num_read;
        {
          string aname;
          bp.copy(op.xattr.name_len, aname);
          string name = "_" + aname;
          int r = getattr_maybe_cache(ctx->obc, name, &outdata);
          if (r >= 0) {
            op.xattr.value_len = r;
            result = 0;
            delta.num_rd_kb += SHIFT_ROUND_UP(r, 10);
            delta.num_rd++;
          }else
          result = r;
        }
        break;
      case CEPH_OSD_OP_GETXATTRS:
        ++ctx->num_read;
        {
          map<string, bufferlist> out;
          result = getattrs_maybe_cache(ctx->obc, &out, true);
          bufferlist bl;
          ::encode(out, bl);
          delta.num_rd_kb += SHIFT_ROUND_UP(bl.length(), 10);
          delta.num_rd++;
          outdata.claim_append(bl);
        }
        break;
      case CEPH_OSD_OP_CMPXATTR:
      case CEPH_OSD_OP_SRC_CMPXATTR:
        ++ctx->num_read;
        {
          string aname;
          bp.copy(op.xattr.name_len, aname);
          string name = "_" + aname;
          bufferlist xattr;
          ObjectContextRef oc = (op.op == CEPH_OSD_OP_CMPXATTR) ? ctx->obc : src_obc;
          result = getattr_maybe_cache(oc, name, &xattr);
          if (result < 0 && result != -EEXIST && result != -ENODATA)
            break;

          switch (op.xattr.cmp_mode) {
            case CEPH_OSD_CMPXATTR_MODE_STRING:
              {
                string val;
                bp.copy(op.xattr.value_len, val);
                val[op.xattr.value_len] = 0;
                dout(10) << "CEPH_OSD_OP_CMPXATTR name=" << name << " val=" << val 
                  << " op=" << (int)op.xattr.cmp_op << " mode=" << (int)op.xattr.cmp_mode << dendl;
                result = do_xattr_cmp_str(op.xattr.cmp_op, val, xattr);
              }
              break;
            case CEPH_OSD_CMPXATTR_MODE_U64:
              {
                uint64_t u64val;
                try {
                  ::decode(u64val, bp);
                  dout(10) << "CEPH_OSD_OP_CMPXATTR name=" << name << " val=" << u64val
                    << " op=" << (int)op.xattr.cmp_op << " mode=" << (int)op.xattr.cmp_mode << dendl;
                  result = do_xattr_cmp_u64(op.xattr.cmp_op, u64val, xattr);
                } catch (buffer::error& e) {
                  result = -EINVAL;
                }
              }
              break;
            default:
              dout(10) << "bad cmp mode " << (int)op.xattr.cmp_mode << dendl;
              result = -EINVAL;
          }

          if (!result) {
            dout(10) << "comparison returned false" << dendl;
            result = -ECANCELED;
            break;
          }
          if (result < 0) {
            dout(10) << "comparison returned " << result << " " << cpp_strerror(-result) << dendl;
            break;
          }

          dout(10) << "comparison returned true" << dendl;
        }
        break;
      case CEPH_OSD_OP_ASSERT_VER:
      case CEPH_OSD_OP_ASSERT_SRC_VERSION:
        ++ctx->num_read;
        {
          uint64_t ver = op.watch.ver;
          if (ver) {
            uint64_t uv = (op.op == CEPH_OSD_OP_ASSERT_VER) ? oi.user_version : src_obc->obs.oi.user_version;
            if (ver < uv)
              result = -ERANGE;
            else if (ver > uv)
              result = -EOVERFLOW;
          } else
            result = -EINVAL;
        }
        break;

        // check for (provisionally) unsupported ops
      case CEPH_OSD_OP_SYNC_READ:
      case CEPH_OSD_OP_MAPEXT:
      case CEPH_OSD_OP_SPARSE_READ:
        derr << "unsupported op: " << *p << dendl;
        assert(false);
      case CEPH_OSD_OP_ISDIRTY:
      case CEPH_OSD_OP_UNDIRTY:
      case CEPH_OSD_OP_CACHE_TRY_FLUSH:
      case CEPH_OSD_OP_CACHE_FLUSH:
      case CEPH_OSD_OP_CACHE_EVICT:
      case CEPH_OSD_OP_LIST_WATCHERS:
      case CEPH_OSD_OP_LIST_SNAPS:
      case CEPH_OSD_OP_NOTIFY:
      case CEPH_OSD_OP_NOTIFY_ACK:
      case CEPH_OSD_OP_SETALLOCHINT:
      case CEPH_OSD_OP_ROLLBACK:
      case CEPH_OSD_OP_CLONERANGE:
      case CEPH_OSD_OP_WATCH:
      case CEPH_OSD_OP_TMAPGET:
      case CEPH_OSD_OP_TMAPPUT:
      case CEPH_OSD_OP_TMAPUP:
      case CEPH_OSD_OP_TMAP2OMAP:
      case CEPH_OSD_OP_OMAPGETKEYS:
      case CEPH_OSD_OP_OMAPGETVALS:
      case CEPH_OSD_OP_OMAPGETHEADER:
      case CEPH_OSD_OP_OMAPGETVALSBYKEYS:
      case CEPH_OSD_OP_OMAP_CMP:
      case CEPH_OSD_OP_OMAPSETVALS:
      case CEPH_OSD_OP_OMAPSETHEADER:
      case CEPH_OSD_OP_OMAPCLEAR:
      case CEPH_OSD_OP_OMAPRMKEYS:
      case CEPH_OSD_OP_COPY_GET_CLASSIC:
      case CEPH_OSD_OP_COPY_GET:
      case CEPH_OSD_OP_COPY_FROM:
        derr << "provisionally unsupported op: " << *p << dendl;
        assert(false);
      case CEPH_OSD_OP_WRITE:
      case CEPH_OSD_OP_WRITEFULL:
      case CEPH_OSD_OP_ZERO:
      case CEPH_OSD_OP_CREATE:
      case CEPH_OSD_OP_TRIMTRUNC:
      case CEPH_OSD_OP_TRUNCATE:
      case CEPH_OSD_OP_DELETE:
      case CEPH_OSD_OP_SETXATTR:
      case CEPH_OSD_OP_RMXATTR:
      case CEPH_OSD_OP_APPEND:
      case CEPH_OSD_OP_STARTSYNC:
        derr << "op should be processed by DirectECProcess: " << *p << dendl;
        assert(false);

      default:
        derr << "unrecognized osd op " << op.op << " " << ceph_osd_op_name(op.op) << dendl;
        result = -EOPNOTSUPP;
    }

    p->rval = result;
    if (result < 0 && (op.flags & CEPH_OSD_OP_FLAG_FAILOK))
      result = 0;

    if (result < 0)
      break;
  }

  if (result >= 0)
    unstable_stats.add(ctx->delta_stats, ctx->obc->obs.oi.category);

  return result;
}

ReplicatedPG::RepGather* ErasureCodePG::new_repop(OpContext* ctx, 
    ObjectContextRef obc, ceph_tid_t rep_tid)
{
  RepGather* repop = new RepGather(ctx, obc, rep_tid, info.last_complete);
  repop->start = ceph_clock_now(cct);
  repop_map[rep_tid] = repop;
  if (ctx->op) {
    req_records[ctx->reqid].rep_tid = rep_tid;
    dout(20) << "insert req_records[" << ctx->reqid << "] = " << rep_tid << dendl;
    size_t max_size = cct->_conf->osd_max_pg_log_entries + 100;
    if (req_records.size() > max_size) {
      size_t num_to_trim = req_records.size() - max_size;
      map<osd_reqid_t, ReqRecord>::iterator itr = req_records.begin();
      while (num_to_trim && itr != req_records.end()) {
        if (itr->second.version != eversion_t()) {
          dout(20) << "erase req_records[" << itr->first << "] = " << itr->second.rep_tid << dendl;
          req_records.erase(itr++);
          --num_to_trim;
        } else
          ++itr;
      }
      if (req_records.size() == max_size + 1)
        dout(5) << "req_records goes beyond " << max_size << dendl;
    }
  }
  
  osd->logger->set(l_osd_op_wip, repop_map.size());
  return repop->get();
}

void ErasureCodePG::eval_repop(RepGather* repop)
{
  OpContext* ctx = repop->ctx;
  MOSDOp* m = NULL;
  if (ctx->op) {
    m = static_cast<MOSDOp *>(ctx->op->get_req());
    dout(10) << "eval_repop " << *repop << " wants=" 
      << (m->wants_ack() ? "a" : "") << (m->wants_ondisk() ? "d" : "")
      << (repop->rep_done ? " DONE" : "") << dendl;
  } else {
    dout(10) << "eval_repop " << *repop << " (no op)" 
      << (repop->rep_done ? " DONE" : "") << dendl;
  }

  if (repop->rep_done)
    return;

  if (m) {
    // an 'ondisk' reply implies 'ack'. so, prefer to send just one
    // ondisk instead of ack followed by ondisk.

    // ondisk?
    if (repop->all_committed) {
      if (!repop->log_op_stat) {
        log_op_stats(ctx);
        publish_stats_to_osd();
        repop->log_op_stat = true;
      }

      // send dup commits, in order
      for (list<OpRequestRef>::iterator i = ctx->waiting_for_ondisk.begin();
          i != ctx->waiting_for_ondisk.end(); ++i) {
        osd->reply_op_error(*i, 0, ctx->at_version, ctx->user_at_version);
      }
      ctx->waiting_for_ondisk.clear();

      // clear out acks, we sent the commits above
      ctx->waiting_for_ack.clear();

      if (m->wants_ondisk() && !repop->sent_disk) {
        // send commit.
        MOSDOpReply *reply = ctx->reply;
        if (reply)
          ctx->reply = NULL;
        else
          reply = new MOSDOpReply(m, 0, get_osdmap()->get_epoch(), 0, true);

        reply->set_reply_versions(ctx->at_version, ctx->user_at_version);
        reply->add_flags(CEPH_OSD_FLAG_ACK | CEPH_OSD_FLAG_ONDISK);
        dout(10) << "sending commit on " << *repop << " " << reply << dendl;
        osd->send_message_osd_client(reply, m->get_connection());
        repop->sent_disk = true;
        ctx->op->mark_commit_sent();
      }
    }

    // applied?
    if (repop->all_applied) {
      // send dup acks, in order
      for (list<OpRequestRef>::iterator i = ctx->waiting_for_ack.begin();
          i != ctx->waiting_for_ack.end(); ++i) {
        MOSDOp* m = static_cast<MOSDOp*>((*i)->get_req());
        MOSDOpReply* reply = new MOSDOpReply(m, 0, get_osdmap()->get_epoch(), 0, true);
        reply->set_reply_versions(ctx->at_version, ctx->user_at_version);
        reply->add_flags(CEPH_OSD_FLAG_ACK);
        osd->send_message_osd_client(reply, m->get_connection());
      }
      ctx->waiting_for_ack.clear();

      if (m->wants_ack() && !repop->sent_ack && !repop->sent_disk) {
        // send ack
        MOSDOpReply *reply = ctx->reply;
        if (reply)
          ctx->reply = NULL;
        else
          reply = new MOSDOpReply(m, 0, get_osdmap()->get_epoch(), 0, true);

        reply->set_reply_versions(ctx->at_version, ctx->user_at_version);
        reply->add_flags(CEPH_OSD_FLAG_ACK);
        dout(10) << "sending ack on " << *repop << " " << reply << dendl;
        assert(entity_name_t::TYPE_OSD != m->get_connection()->peer_type);
        osd->send_message_osd_client(reply, m->get_connection());
        repop->sent_ack = true;
      }

      // note the write is now readable (for rlatency calc).  note
      // that this will only be defined if the write is readable
      // _prior_ to being committed; it will not get set with
      // writeahead journaling, for instance.
      if (ctx->readable_stamp == utime_t())
        ctx->readable_stamp = ceph_clock_now(cct);
    }
  }

  // done.
  if (repop->all_applied && repop->all_committed) {
    repop->rep_done = true;

    calc_min_last_complete_ondisk();

    // kick snap_trimmer if necessary
    if (repop->queue_snap_trimmer) {
      queue_snap_trim();
    }

    // set version for related ReqRecord
    if (ctx->op) {
      osd_reqid_t reqid = ctx->op->get_reqid();
      map<osd_reqid_t, ReqRecord>::iterator itr = req_records.find(reqid);
      if (itr == req_records.end()) {
        derr << "req_records[" << reqid << "] does not exist!" << dendl;
        assert(false);
      }
      
      ReqRecord& record = itr->second;
      assert(record.version == eversion_t());
      assert(record.user_version == 0);
      
      record.version = ctx->at_version;
      record.user_version = ctx->user_at_version;
      dout(20) << "req_records[" << reqid << "] = " << record.rep_tid
           << ", set v " << record.version << " uv " << record.user_version << dendl;
    }
    
    dout(10) << "removing " << *repop << dendl;
    remove_repop(repop);
  }
}

bool ErasureCodePG::check_dup_op(OpRequestRef& op)
{
  MOSDOp* m = static_cast<MOSDOp*>(op->get_req());
  osd_reqid_t reqid = m->get_reqid();
  map<osd_reqid_t, ReqRecord>::iterator rec_itr = req_records.find(reqid);
  if (rec_itr == req_records.end()) {
    return false;
  }

  ReqRecord& record = rec_itr->second;
  dout(0) << __func__ << " dup " << reqid << " was " << record.rep_tid << dendl;
  // version is not NULL, means it is completed
  if (record.version != eversion_t()) {
    osd->reply_op_error(op, 0, record.version, record.user_version);
    return true;
  }

  map<ceph_tid_t, RepGather*>::iterator rep_itr = repop_map.find(record.rep_tid);
  assert(rep_itr != repop_map.end());
  RepGather* repop = rep_itr->second;
  OpContext* ctx = repop->ctx;

  if (m->wants_ack()) {
    if (repop->all_applied) {
      MOSDOpReply* reply = new MOSDOpReply(m, 0, get_osdmap()->get_epoch(), 0, false);
      reply->add_flags(CEPH_OSD_FLAG_ACK);
      reply->set_reply_versions(ctx->at_version, ctx->user_at_version);
      osd->send_message_osd_client(reply, m->get_connection());
    } else {
      dout(0) << "waiting for ack" << dendl;
      ctx->waiting_for_ack.push_back(op);
    }
  }

  dout(0) << "waiting for ondisk" << dendl;
  ctx->waiting_for_ondisk.push_back(op);
  op->mark_delayed("waiting for ondisk");
  return true;
}

void ErasureCodePG::apply_and_flush_repops(bool requeue)
{
  decp->on_change();

  list<OpRequestRef> rq;
  map<ceph_tid_t, RepGather*>::iterator itr = repop_map.begin();
  while (itr != repop_map.end())
  {
    RepGather* repop = itr->second;
    dout(0) << "canceling repop tid " << itr->first << dendl;
    repop->rep_aborted = true;
    if (repop->on_applied) {
      delete repop->on_applied;
      repop->on_applied = NULL;
    }

    if (requeue) {
      OpRequestRef op = repop->ctx->op;
      if (op) {
        dout(10) << "requeuing " << *(op->get_req()) << dendl;
        rq.push_back(op);
      }

      list<OpRequestRef>& wq = repop->ctx->waiting_for_ondisk;
      for (list<OpRequestRef>::iterator i = wq.begin(); i != wq.end(); ++i) {
        dout(10) << "also requeuing ondisk_waiters " << *i << dendl;
        rq.push_back(*i);
      }
    }
    
    ++itr;
    remove_repop(repop);
  }

  assert(repop_map.empty());
  // delete all the records of uncompleted repops
  map<osd_reqid_t, ReqRecord>::iterator i = req_records.begin();
  while (i != req_records.end()) {
    if (i->second.version == eversion_t())
      req_records.erase(i++);
    else
      ++i;
  }

  if (requeue)
    requeue_ops(rq);
}

struct C_OSD_LocalRecoverDone: public Context {
  ErasureCodePGRef pg;
  hobject_t hoid;
  eversion_t version;

  C_OSD_LocalRecoverDone(ErasureCodePG* _pg, const hobject_t& _hoid, const eversion_t& _version)
    :pg(_pg), hoid(_hoid), version(_version) {}

  void finish(int r) {
    pg->local_recover_done(hoid, version);
  }
};

void ErasureCodePG::on_local_recover(
  const hobject_t& hoid,
  const object_stat_sum_t& stat_diff,
  const ObjectRecoveryInfo& _recovery_info,
  ObjectContextRef obc,
  ObjectStore::Transaction* t
  )
{
  dout(10) << __func__ << ": " << hoid << dendl;
  ObjectRecoveryInfo recovery_info(_recovery_info);
  if (recovery_info.soid.snap < CEPH_NOSNAP) {
    assert(recovery_info.oi.snaps.size());
    OSDriver::OSTransaction _t(osdriver.get_transaction(t));
    set<snapid_t> snaps(
      recovery_info.oi.snaps.begin(),
      recovery_info.oi.snaps.end());
    snap_mapper.add_oid(
      recovery_info.soid,
      snaps,
      &_t);
  }

  if (pg_log.get_missing().is_missing(recovery_info.soid) &&
      pg_log.get_missing().missing.find(recovery_info.soid)->second.need > recovery_info.version) {
    assert(is_primary());
    const pg_log_entry_t *latest = pg_log.get_log().objects.find(recovery_info.soid)->second;
    if (latest->op == pg_log_entry_t::LOST_REVERT &&
	latest->reverting_to == recovery_info.version) {
      dout(10) << " got old revert version " << recovery_info.version
	       << " for " << *latest << dendl;
      recovery_info.version = latest->version;
      // update the attr to the revert event version
      recovery_info.oi.prior_version = recovery_info.oi.version;
      recovery_info.oi.version = latest->version;
      bufferlist bl;
      ::encode(recovery_info.oi, bl);
      t->setattr(coll, recovery_info.soid, OI_ATTR, bl);
      if (obc)
	obc->attr_cache[OI_ATTR] = bl;
    }
  }

  // keep track of active pushes for scrub
  ++active_pushes;

  if (is_primary()) {
    info.stats.stats.sum.add(stat_diff);

    assert(obc);
    obc->obs.exists = true;
    obc->ondisk_write_lock();
    obc->obs.oi = recovery_info.oi;  // may have been updated above


    t->register_on_applied(new C_OSD_AppliedRecoveredObject(this, obc));
    t->register_on_applied_sync(new C_OSD_OndiskWriteUnlock(obc));

    publish_stats_to_osd();
    assert(missing_loc.needs_recovery(hoid));
    t->register_on_applied(bless_context(
       new C_OSD_LocalRecoverDone(this, hoid, recovery_info.version)));
  } else {
    recover_got(recovery_info.soid, recovery_info.version);
    t->register_on_applied(
      new C_OSD_AppliedRecoveredObjectReplica(this));

  }

  t->register_on_commit(
    new C_OSD_CommittedPushedObject(
      this,
      get_osdmap()->get_epoch(),
      info.last_complete));

  // update pg
  dirty_info = true;
  write_if_dirty(*t);

}
void ErasureCodePG::local_recover_done(const hobject_t& hoid, const eversion_t& version)
{
  recover_got(hoid, version);
  missing_loc.add_location(hoid, pg_whoami);
  if (!is_unreadable_object(hoid) && waiting_for_unreadable_object.count(hoid)) {
    dout(5) << __func__ << " kicking unreadable waiters on " << hoid << dendl;
    requeue_ops(waiting_for_unreadable_object[hoid]);
    waiting_for_unreadable_object.erase(hoid);
  }
  if (pg_log.get_missing().missing.size() == 0) {
    requeue_ops(waiting_for_all_missing);
    waiting_for_all_missing.clear();
  }
}
void ErasureCodePG::do_op(OpRequestRef& op)
{
  MOSDOp *m = static_cast<MOSDOp*>(op->get_req());
  assert(m->get_header().type == CEPH_MSG_OSD_OP);
  if (op->includes_pg_op()) {
    if (pg_op_must_wait(m)) {
      wait_for_all_missing(op);
      return;
    }
    return do_pg_op(op);
  }

  if (get_osdmap()->is_blacklisted(m->get_source_addr())) {
    dout(10) << "do_op " << m->get_source_addr() << " is blacklisted" << dendl;
    osd->reply_op_error(op, -EBLACKLISTED);
    return;
  }

  // order this op as a write?
  bool write_ordered =
    op->may_write() ||
    op->may_cache() ||
    (m->get_flags() & CEPH_OSD_FLAG_RWORDERED);

  dout(10) << "do_op " << *m
	   << (op->may_write() ? " may_write" : "")
	   << (op->may_read() ? " may_read" : "")
	   << (op->may_cache() ? " may_cache" : "")
	   << " -> " << (write_ordered ? "write-ordered" : "read-ordered")
	   << " flags " << ceph_osd_flag_string(m->get_flags())
	   << dendl;

  hobject_t head(m->get_oid(), m->get_object_locator().key,
		 CEPH_NOSNAP, m->get_pg().ps(),
		 info.pgid.pool(), m->get_object_locator().nspace);


  if (write_ordered && scrubber.write_blocked_by_scrub(head)) {
    dout(20) << __func__ << ": waiting for scrub" << dendl;
    waiting_for_active.push_back(op);
    op->mark_delayed("waiting for scrub");
    return;
  }

  // missing object?
  if (is_unreadable_object(head)) {
    wait_for_unreadable_object(head, op);
    return;
  }
  // degraded object?
  if (write_ordered && is_degraded_object(head)) {
    wait_for_degraded_object(head, op);
    return;
  }

  // missing snapdir?
  hobject_t snapdir(m->get_oid(), m->get_object_locator().key,
		    CEPH_SNAPDIR, m->get_pg().ps(), info.pgid.pool(),
		    m->get_object_locator().nspace);
  if (is_unreadable_object(snapdir)) {
    wait_for_unreadable_object(snapdir, op);
    return;
  }

  // degraded object?
  if (write_ordered && is_degraded_object(snapdir)) {
    wait_for_degraded_object(snapdir, op);
    return;
  }
 
  // asking for SNAPDIR is only ok for reads
  if (m->get_snapid() == CEPH_SNAPDIR && op->may_write()) {
    osd->reply_op_error(op, -EINVAL);
    return;
  }

  // dup/replay? // wumq modified for ErasureCodePG
  if ((op->may_write() || op->may_cache()) && check_dup_op(op)) {
    return;
  }

  ObjectContextRef obc;
  bool can_create = op->may_write() || op->may_cache();
  hobject_t missing_oid;
  hobject_t oid(m->get_oid(),
		m->get_object_locator().key,
		m->get_snapid(),
		m->get_pg().ps(),
		m->get_object_locator().get_pool(),
		m->get_object_locator().nspace);

  // io blocked on obc?
  if (((m->get_flags() & CEPH_OSD_FLAG_FLUSH) == 0) &&
      maybe_await_blocked_snapset(oid, op)) {
    return;
  }

  int r = find_object_context(
    oid, &obc, can_create,
    m->get_flags() & CEPH_OSD_FLAG_MAP_SNAP_CLONE,
    &missing_oid);

  if (r == -EAGAIN) {
    // If we're not the primary of this OSD, and we have
    // CEPH_OSD_FLAG_LOCALIZE_READS set, we just return -EAGAIN. Otherwise,
    // we have to wait for the object.
    if (is_primary() ||
	(!(m->get_flags() & CEPH_OSD_FLAG_BALANCE_READS) &&
	 !(m->get_flags() & CEPH_OSD_FLAG_LOCALIZE_READS))) {
      // missing the specific snap we need; requeue and wait.
      assert(!op->may_write()); // only happens on a read/cache
      wait_for_unreadable_object(missing_oid, op);
      return;
    }
  } else if (r == 0) {
    if (is_unreadable_object(obc->obs.oi.soid)) {
      dout(10) << __func__ << ": clone " << obc->obs.oi.soid
	       << " is unreadable, waiting" << dendl;
      wait_for_unreadable_object(obc->obs.oi.soid, op);
      return;
    }

    // degraded object?  (the check above was for head; this could be a clone)
    if (write_ordered &&
	obc->obs.oi.soid.snap != CEPH_NOSNAP &&
  is_degraded_object(obc->obs.oi.soid)) {
      dout(10) << __func__ << ": clone " << obc->obs.oi.soid
	       << " is degraded, waiting" << dendl;
      wait_for_degraded_object(obc->obs.oi.soid, op);
      return;
    }
  }

  bool in_hit_set = false;
  if (hit_set) {
    if (missing_oid != hobject_t() && hit_set->contains(missing_oid))
      in_hit_set = true;
    hit_set->insert(oid);
    if (hit_set->is_full() ||
	hit_set_start_stamp + pool.info.hit_set_period <= m->get_recv_stamp()) {
      hit_set_persist();
    }
  }

  if (agent_state) {
    agent_choose_mode();
  }

  if ((m->get_flags() & CEPH_OSD_FLAG_IGNORE_CACHE) == 0 &&
      maybe_handle_cache(op, write_ordered, obc, r, missing_oid, false, in_hit_set))
    return;

  if (r) {
    osd->reply_op_error(op, r);
    return;
  }

  // make sure locator is consistent
  object_locator_t oloc(obc->obs.oi.soid);
  if (m->get_object_locator() != oloc) {
    dout(10) << " provided locator " << m->get_object_locator() 
	     << " != object's " << obc->obs.oi.soid << dendl;
    osd->clog->warn() << "bad locator " << m->get_object_locator() 
		     << " on object " << oloc
		     << " op " << *m << "\n";
  }

  // io blocked on obc?
  if (obc->is_blocked() &&
      (m->get_flags() & CEPH_OSD_FLAG_FLUSH) == 0) {
    wait_for_blocked_object(obc->obs.oi.soid, op);
    return;
  }

  dout(25) << __func__ << " oi " << obc->obs.oi << dendl;
  // are writes blocked by another object?
  if (obc->blocked_by) {
    dout(10) << "do_op writes for " << obc->obs.oi.soid << " blocked by "
	     << obc->blocked_by->obs.oi.soid << dendl;
    wait_for_degraded_object(obc->blocked_by->obs.oi.soid, op);
    return;
  }

  // src_oids
  map<hobject_t,ObjectContextRef> src_obc;
  for (vector<OSDOp>::iterator p = m->ops.begin(); p != m->ops.end(); ++p) {
    OSDOp& osd_op = *p;

    // make sure LIST_SNAPS is on CEPH_SNAPDIR and nothing else
    if (osd_op.op.op == CEPH_OSD_OP_LIST_SNAPS &&
	m->get_snapid() != CEPH_SNAPDIR) {
      dout(10) << "LIST_SNAPS with incorrect context" << dendl;
      osd->reply_op_error(op, -EINVAL);
      return;
    }

    if (!ceph_osd_op_type_multi(osd_op.op.op))
      continue;
    if (osd_op.soid.oid.name.length()) {
      object_locator_t src_oloc;
      get_src_oloc(m->get_oid(), m->get_object_locator(), src_oloc);
      hobject_t src_oid(osd_op.soid, src_oloc.key, m->get_pg().ps(),
			info.pgid.pool(), m->get_object_locator().nspace);
      if (!src_obc.count(src_oid)) {
	ObjectContextRef sobc;
	hobject_t wait_oid;
	int r;

	if (src_oid.is_head() && is_missing_object(src_oid)) {
	  wait_for_unreadable_object(src_oid, op);
	} else if ((r = find_object_context(
		      src_oid, &sobc, false, false,
		      &wait_oid)) == -EAGAIN) {
	  // missing the specific snap we need; requeue and wait.
	  wait_for_unreadable_object(wait_oid, op);
	} else if (r) {
	  if (!maybe_handle_cache(op, write_ordered, sobc, r, wait_oid, true))
	    osd->reply_op_error(op, r);
	} else if (sobc->obs.oi.is_whiteout()) {
	  osd->reply_op_error(op, -ENOENT);
	} else {
	  if (sobc->obs.oi.soid.get_key() != obc->obs.oi.soid.get_key() &&
		   sobc->obs.oi.soid.get_key() != obc->obs.oi.soid.oid.name &&
		   sobc->obs.oi.soid.oid.name != obc->obs.oi.soid.get_key()) {
	    dout(1) << " src_oid " << sobc->obs.oi.soid << " != "
		  << obc->obs.oi.soid << dendl;
	    osd->reply_op_error(op, -EINVAL);
	  } else if (is_degraded_object(sobc->obs.oi.soid) ||
		   (check_src_targ(sobc->obs.oi.soid, obc->obs.oi.soid))) {
	    if (is_degraded_object(sobc->obs.oi.soid)) {
	      wait_for_degraded_object(sobc->obs.oi.soid, op);
	    } else {
	      waiting_for_degraded_object[sobc->obs.oi.soid].push_back(op);
	      op->mark_delayed("waiting for degraded object");
	    }
	    dout(10) << " writes for " << obc->obs.oi.soid << " now blocked by "
		     << sobc->obs.oi.soid << dendl;
	    obc->blocked_by = sobc;
	    sobc->blocking.insert(obc);
	  } else {
	    dout(10) << " src_oid " << src_oid << " obc " << src_obc << dendl;
	    src_obc[src_oid] = sobc;
	    continue;
	  }
	}
	// Error cleanup below
      } else {
	continue;
      }
      // Error cleanup below
    } else {
      dout(10) << "no src oid specified for multi op " << osd_op << dendl;
      osd->reply_op_error(op, -EINVAL);
    }
    return;
  }

  // any SNAPDIR op needs to have all clones present.  treat them as
  // src_obc's so that we track references properly and clean up later.
  if (m->get_snapid() == CEPH_SNAPDIR) {
    for (vector<snapid_t>::iterator p = obc->ssc->snapset.clones.begin();
	 p != obc->ssc->snapset.clones.end();
	 ++p) {
      hobject_t clone_oid = obc->obs.oi.soid;
      clone_oid.snap = *p;
      if (!src_obc.count(clone_oid)) {
	if (is_unreadable_object(clone_oid)) {
	  wait_for_unreadable_object(clone_oid, op);
	  return;
	}

	ObjectContextRef sobc = get_object_context(clone_oid, false);
	if (!sobc) {
	  if (!maybe_handle_cache(op, write_ordered, sobc, -ENOENT, clone_oid, true))
	    osd->reply_op_error(op, -ENOENT);
	  return;
	} else {
	  dout(10) << " clone_oid " << clone_oid << " obc " << sobc << dendl;
	  src_obc[clone_oid] = sobc;
	  continue;
	}
	assert(0); // unreachable
      } else {
	continue;
      }
    }
  }

  OpContext *ctx = new OpContext(op, m->get_reqid(), m->ops,
				 &obc->obs, obc->ssc, 
				 this);
  ctx->op_t = pgbackend->get_transaction();
  ctx->obc = obc;

  if (!obc->obs.exists)
    ctx->snapset_obc = get_object_context(obc->obs.oi.soid.get_snapdir(), false);

  if (m->get_flags() & CEPH_OSD_FLAG_SKIPRWLOCKS) {
    dout(20) << __func__ << ": skipping rw locks" << dendl;
  } else if (m->get_flags() & CEPH_OSD_FLAG_FLUSH) {
    dout(20) << __func__ << ": part of flush, will ignore write lock" << dendl;

    // verify there is in fact a flush in progress
    // FIXME: we could make this a stronger test.
    map<hobject_t,FlushOpRef>::iterator p = flush_ops.find(obc->obs.oi.soid);
    if (p == flush_ops.end()) {
      dout(10) << __func__ << " no flush in progress, aborting" << dendl;
      reply_ctx(ctx, -EINVAL);
      return;
    }
  } else if (!get_rw_locks(ctx)) {
    dout(20) << __func__ << " waiting for rw locks " << dendl;
    op->mark_delayed("waiting for rw locks");
    close_op_ctx(ctx, -EBUSY);
    return;
  }

  if ((op->may_read()) && (obc->obs.oi.is_lost())) {
    // This object is lost. Reading from it returns an error.
    dout(20) << __func__ << ": object " << obc->obs.oi.soid
	     << " is lost" << dendl;
    reply_ctx(ctx, -ENFILE);
    return;
  }
  if (!op->may_write() &&
      !op->may_cache() &&
      (!obc->obs.exists ||
       ((m->get_snapid() != CEPH_SNAPDIR) &&
	obc->obs.oi.is_whiteout()))) {
    reply_ctx(ctx, -ENOENT);
    return;
  }

  op->mark_started();
  ctx->src_obc = src_obc;

  execute_ctx(ctx);
}

