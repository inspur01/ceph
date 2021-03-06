// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Portions Copyright (C) 2013 CohortFS, LLC
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <arpa/inet.h>
#include <boost/lexical_cast.hpp>
#include <set>
#include <stdlib.h>
#include <memory>

#include "XioMsg.h"
#include "XioMessenger.h"
#include "common/address_helper.h"
#include "common/code_environment.h"
#include "common/ms_status.h"
#include "messages/MNop.h"

#define dout_subsys ceph_subsys_xio

#define safe_free(x)	if (x) { free(x); x = NULL; }

Mutex mtx("XioMessenger Package Lock");
atomic_t initialized;

atomic_t XioMessenger::nInstances;

struct xio_mempool *xio_msgr_noreg_mpool;

static struct xio_session_ops xio_msgr_ops;

/* Accelio API callouts */

/* string table */
static const char *xio_session_event_types[] =
{ "XIO_SESSION_REJECT_EVENT",
  "XIO_SESSION_TEARDOWN_EVENT",
  "XIO_SESSION_NEW_CONNECTION_EVENT",
  "XIO_SESSION_CONNECTION_ESTABLISHED_EVENT",
  "XIO_SESSION_CONNECTION_TEARDOWN_EVENT",
  "XIO_SESSION_CONNECTION_CLOSED_EVENT",
  "XIO_SESSION_CONNECTION_DISCONNECTED_EVENT",
  "XIO_SESSION_CONNECTION_REFUSED_EVENT",
  "XIO_SESSION_CONNECTION_ERROR_EVENT",
  "XIO_SESSION_ERROR_EVENT"
};

namespace xio_log
{
typedef pair<const char*, int> level_pair;
static const level_pair LEVELS[] = {
  make_pair("fatal", 0),
  make_pair("error", 0),
  make_pair("warn", 1),
  make_pair("info", 1),
  make_pair("debug", 2),
  make_pair("trace", 20)
};

// maintain our own global context, we can't rely on g_ceph_context
// for things like librados
static CephContext *context;

int get_level()
{
  int level = 0;
  for (size_t i = 0; i < sizeof(LEVELS); i++) {
    if (!ldlog_p1(context, dout_subsys, LEVELS[i].second))
      break;
    level++;
  }
  return level;
}

void log_dout(const char *file, unsigned line,
	      const char *function, unsigned level,
	      const char *fmt, ...)
{
  char buffer[2048];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  if (n > 0) {
    const char *short_file = strrchr(file, '/');
    short_file = (short_file == NULL) ? file : short_file + 1;

    const level_pair &lvl = LEVELS[level];
    ldout(context, lvl.second) << '[' << lvl.first << "] "
      << short_file << ':' << line << ' '
      << function << " - " << buffer << dendl;
  }
}
}

static int on_session_event(struct xio_session *session,
			    struct xio_session_event_data *event_data,
			    void *cb_user_context)
{
  XioMessenger *msgr = static_cast<XioMessenger*>(cb_user_context);
  CephContext *cct = msgr->cct;

  ldout(cct,4) << "session event: " << xio_session_event_str(event_data->event)
    << ". reason: " << xio_strerror(event_data->reason) << dendl;

  return msgr->session_event(session, event_data, cb_user_context);
}

static int on_new_session(struct xio_session *session,
			  struct xio_new_session_req *req,
			  void *cb_user_context)
{
  XioMessenger *msgr = static_cast<XioMessenger*>(cb_user_context);
  CephContext *cct = msgr->cct;

  ldout(cct,4) << "new session " << session
    << " user_context " << cb_user_context << dendl;

  return (msgr->new_session(session, req, cb_user_context));
}

static inline XioDispatchHook* pool_alloc_xio_dispatch_hook2(
				XioConnection *xcon, Message *m, XioInSeq& msg_seq)
{
		struct xio_reg_mem mp_mem;
		int e = xpool_alloc(xio_msgr_noreg_mpool,
						sizeof(XioDispatchHook), &mp_mem);
		if (!!e)
				return NULL;
		XioDispatchHook *xhook = static_cast<XioDispatchHook*>(mp_mem.addr);
		new (xhook) XioDispatchHook(xcon, m, msg_seq, mp_mem);
		return xhook;
}

static int on_msg(struct xio_session *session,
		  struct xio_msg *req,
		  int more_in_batch,
		  void *cb_user_context)
{
  XioMessenger* pmsgr = NULL;		
  PROTOCOL_PARSER_STRU_T* pstrutask = NULL;
  XioConnection* xcon __attribute__((unused)) = static_cast<XioConnection*>(cb_user_context);
  CephContext *cct = xcon->get_messenger()->cct;
  pmsgr = (XioMessenger*)xcon->get_messenger();

  ldout(cct,25) << "on_msg session " << session << " xcon " << xcon << dendl;
  if (unlikely(XioPool::trace_mempool)) {
    static uint32_t nreqs;
    if (unlikely((++nreqs % 65536) == 0)) {
      xp_stats.dump(__func__, nreqs);
    }
  }

  struct xio_msg *treq = req;
  /* XXX Accelio guarantees message ordering at
   *   * xio_session */
  if (! xcon->in_seq.p()) {
		  if (!treq->in.header.iov_len) {
				  ldout(pmsgr->cct,0) << __func__ << " empty header: packet out of sequence?" << dendl;
				  xio_release_msg(req);
				  return 0;
		  }
		  XioMsgCnt msg_cnt( buffer::create_static(treq->in.header.iov_len, (char*) treq->in.header.iov_base));
		  ldout(pmsgr->cct,10) << __func__ << " receive req " << "treq " << treq
				  << " msg_cnt " << msg_cnt.msg_cnt
				  << " iov_base " << treq->in.header.iov_base
				  << " iov_len " << (int) treq->in.header.iov_len
				  << " nents " << treq->in.pdata_iov.nents
				  << " conn " << xcon << " sess " << session
				  << " sn " << treq->sn << dendl;
		  assert(session == xcon->session);
		  xcon->in_seq.set_count(msg_cnt.msg_cnt);
  } else {
		  /* XXX major sequence error */
		  assert(!treq->in.header.iov_len);
  }

  xcon->in_seq.append(req);
  if (xcon->in_seq.count() > 0) {
		  return 0;
  }

  XioDispatchHook *m_hook = pool_alloc_xio_dispatch_hook2(xcon, NULL /* msg */, xcon->in_seq);
  xcon->in_seq.clear();
  
#if 0
  pstrutask = (PROTOCOL_PARSER_STRU_T*)malloc(1*sizeof(PROTOCOL_PARSER_STRU_T));
  pstrutask->m_hook = m_hook;
  pstrutask->xcon = xcon;

  pmsgr->queueparser->Push(pstrutask);
  return 0;
#endif
  return xcon->on_msg_req2(m_hook);
  //return xcon->on_msg_req(session, req, more_in_batch, cb_user_context);
}

static int on_ow_msg_send_complete(struct xio_session *session,
				   struct xio_msg *msg,
				   void *conn_user_context)
{
  XioConnection *xcon =
    static_cast<XioConnection*>(conn_user_context);
  CephContext *cct = xcon->get_messenger()->cct;

  ldout(cct,25) << "msg delivered session: " << session
		<< " msg: " << msg << " conn_user_context "
		<< conn_user_context << dendl;

  return xcon->on_ow_msg_send_complete(session, msg, conn_user_context);
}

static int on_msg_error(struct xio_session *session,
			enum xio_status error,
			enum xio_msg_direction dir,
			struct xio_msg  *msg,
			void *conn_user_context)
{
  /* XIO promises to flush back undelivered messages */
  XioConnection *xcon =
    static_cast<XioConnection*>(conn_user_context);
  CephContext *cct = xcon->get_messenger()->cct;

  ldout(cct,4) << "msg error session: " << session
    << " error: " << xio_strerror(error) << " msg: " << msg
    << " conn_user_context " << conn_user_context << dendl;

  return xcon->on_msg_error(session, error, msg, conn_user_context);
}

static int on_cancel(struct xio_session *session,
		     struct xio_msg  *msg,
		     enum xio_status result,
		     void *conn_user_context)
{
  XioConnection* xcon __attribute__((unused)) =
    static_cast<XioConnection*>(conn_user_context);
  CephContext *cct = xcon->get_messenger()->cct;

  ldout(cct,25) << "on cancel: session: " << session << " msg: " << msg
    << " conn_user_context " << conn_user_context << dendl;

  return 0;
}

static int on_cancel_request(struct xio_session *session,
			     struct xio_msg  *msg,
			     void *conn_user_context)
{
  XioConnection* xcon __attribute__((unused)) =
    static_cast<XioConnection*>(conn_user_context);
  CephContext *cct = xcon->get_messenger()->cct;

  ldout(cct,25) << "on cancel request: session: " << session << " msg: " << msg
    << " conn_user_context " << conn_user_context << dendl;

  return 0;
}

/* free functions */
static string xio_uri_from_entity(const string &type,
				  const entity_addr_t& addr, bool want_port)
{
  const char *host = NULL;
  char addr_buf[129];
  string xio_uri;

  switch(addr.addr.ss_family) {
  case AF_INET:
    host = inet_ntop(AF_INET, &addr.addr4.sin_addr, addr_buf,
		     INET_ADDRSTRLEN);
    break;
  case AF_INET6:
    host = inet_ntop(AF_INET6, &addr.addr6.sin6_addr, addr_buf,
		     INET6_ADDRSTRLEN);
    break;
  default:
    abort();
    break;
  };

  if (type == "rdma" || type == "tcp")
      xio_uri = type + "://";
  else
      xio_uri = "rdma://";

  /* The following can only succeed if the host is rdma-capable */
  xio_uri += host;
  if (want_port) {
    xio_uri += ":";
    xio_uri += boost::lexical_cast<std::string>(addr.get_port());
  }

  return xio_uri;
} /* xio_uri_from_entity */

void XioInit::package_init(CephContext *cct) {
   if (! initialized.read()) {

     mtx.Lock();
     if (! initialized.read()) {

       xio_init();

       // claim a reference to the first context we see
       xio_log::context = cct->get();

       int xopt;
       xopt = xio_log::get_level();
       xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_LOG_LEVEL,
 		  &xopt, sizeof(xopt));
       xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_LOG_FN,
 		  (const void*)xio_log::log_dout, sizeof(xio_log_fn));

       xopt = 1;
       xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_DISABLE_HUGETBL,
 		  &xopt, sizeof(xopt));

       if (g_code_env == CODE_ENVIRONMENT_DAEMON) {
         xopt = 1;
         xio_set_opt(NULL, XIO_OPTLEVEL_RDMA, XIO_OPTNAME_ENABLE_FORK_INIT,
 		    &xopt, sizeof(xopt));
       }

       xopt = XIO_MSGR_IOVLEN;
       xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_MAX_IN_IOVLEN,
 		  &xopt, sizeof(xopt));
       xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_MAX_OUT_IOVLEN,
 		  &xopt, sizeof(xopt));

       /* enable flow-control */
       xopt = 1;
       xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_ENABLE_FLOW_CONTROL,
                  &xopt, sizeof(xopt));

       /* and set threshold for buffer callouts */
       xopt = max(cct->_conf->xio_max_send_inline, 512);
       xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_MAX_INLINE_XIO_DATA,
                  &xopt, sizeof(xopt));
       /* ceph msg header struct len */
       xopt = 216;
       xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_MAX_INLINE_XIO_HEADER,
                  &xopt, sizeof(xopt));

       size_t queue_depth = cct->_conf->xio_queue_depth;
       struct xio_mempool_config mempool_config = {
         6,
         {
           {1024,  0,  queue_depth,  262144},
           {4096,  0,  queue_depth,  262144},
           {16384, 0,  queue_depth,  262144},
           {65536, 0,  128,  65536},
           {262144, 0,  32,  16384},
           {1048576, 0, 8,  8192}
         }
       };
       xio_set_opt(NULL,
                   XIO_OPTLEVEL_ACCELIO, XIO_OPTNAME_CONFIG_MEMPOOL,
                   &mempool_config, sizeof(mempool_config));

       /* and unregisterd one */
 #define XMSG_MEMPOOL_QUANTUM 4096

       xio_msgr_noreg_mpool =
 	xio_mempool_create(-1 /* nodeid */,
 			   XIO_MEMPOOL_FLAG_REGULAR_PAGES_ALLOC);

       (void) xio_mempool_add_slab(xio_msgr_noreg_mpool, 64,
 				       cct->_conf->xio_mp_min,
 				       cct->_conf->xio_mp_max_64,
 				       XMSG_MEMPOOL_QUANTUM, 0);
       (void) xio_mempool_add_slab(xio_msgr_noreg_mpool, 256,
 				       cct->_conf->xio_mp_min,
 				       cct->_conf->xio_mp_max_256,
 				       XMSG_MEMPOOL_QUANTUM, 0);
       (void) xio_mempool_add_slab(xio_msgr_noreg_mpool, 1024,
 				       cct->_conf->xio_mp_min,
 				       cct->_conf->xio_mp_max_1k,
 				       XMSG_MEMPOOL_QUANTUM, 0);
       (void) xio_mempool_add_slab(xio_msgr_noreg_mpool, getpagesize(),
 				       cct->_conf->xio_mp_min,
 				       cct->_conf->xio_mp_max_page,
 				       XMSG_MEMPOOL_QUANTUM, 0);

       /* initialize ops singleton */
       xio_msgr_ops.on_session_event = on_session_event;
       xio_msgr_ops.on_new_session = on_new_session;
       xio_msgr_ops.on_session_established = NULL;
       xio_msgr_ops.on_msg = on_msg;
       xio_msgr_ops.on_ow_msg_send_complete = on_ow_msg_send_complete;
       xio_msgr_ops.on_msg_error = on_msg_error;
       xio_msgr_ops.on_cancel = on_cancel;
       xio_msgr_ops.on_cancel_request = on_cancel_request;

       /* mark initialized */
       initialized.set(1);
     }
     mtx.Unlock();
   }
 }

/* XioMessenger */
XioMessenger::XioMessenger(CephContext *cct, entity_name_t name,
			   string mname, uint64_t _nonce, uint64_t features,
			   DispatchStrategy *ds)
  : SimplePolicyMessenger(cct, name, mname, _nonce),
    XioInit(cct),
    nsessions(0),
    shutdown_called(false),
    portals(this, get_nportals(mname), get_nconns_per_portal(mname)),
    dispatch_strategy(ds),
    loop_con(new XioLoopbackConnection(this)),
    special_handling(0),
    sh_mtx("XioMessenger session mutex"),
    gc_mtx("XioMessenger getconenction mutex"),
    sh_cond(),
    need_addr(true),
    did_bind(false),
    nonce(_nonce),
    event_finisher(cct)
{

  if (cct->_conf->xio_trace_xcon)
    magic |= MSG_MAGIC_TRACE_XCON;

  XioPool::trace_mempool = (cct->_conf->xio_trace_mempool);
  XioPool::trace_msgcnt = (cct->_conf->xio_trace_msgcnt);

  dispatch_strategy->set_messenger(this);
  queueparser = new QueueParser(1);
  queueparser->start();
  /* update class instance count */
  nInstances.inc();

  local_features = features;
  loop_con->set_features(features);

  ldout(cct,2) << "Create msgr: " << this << " instance: "
    << nInstances.read() << " type: " << name.type_str()
    << " subtype: " << mname << " nportals: " << get_nportals(mname)
    << " nconns_per_portal: " << get_nconns_per_portal(mname) << " features: "
    << features << dendl;

} /* ctor */

int XioMessenger::pool_hint(uint32_t dsize) {
  if (dsize > 1024*1024)
    return 0;

  /* if dsize is already present, returns -EEXIST */
  return xio_mempool_add_slab(xio_msgr_noreg_mpool, dsize, 0,
				   cct->_conf->xio_mp_max_hint,
				   XMSG_MEMPOOL_QUANTUM, 0);
}

int XioMessenger::get_nconns_per_portal(const string &mname)
{
  int nconns_per_portal;

  if (mname == "client" || mname == "cluster" || mname == "mon")
    nconns_per_portal = max(cct->_conf->xio_max_conns_per_portal, 100);
  else if (mname == "hb_back_server" || mname == "hb_back_front" ||
           mname == "hbclient")
    //nconns_per_portal = cct->_conf->osd_heartbeat_min_peers;
    nconns_per_portal = 100;
  else /* osd's ms_objecter or client messenger */
    nconns_per_portal = min(cct->_conf->xio_max_conns_per_portal, 32);

  return nconns_per_portal;
}

int XioMessenger::get_nportals(const string &mname)
{
  int nportals = 1;

  if (mname == "client" || mname == "cluster")
    nportals = max(cct->_conf->xio_portal_threads, 1);

  return nportals;
}

void XioMessenger::learned_addr(const entity_addr_t &peer_addr_for_me)
{
  // be careful here: multiple threads may block here, and readers of
  // my_inst.addr do NOT hold any lock.

  // this always goes from true -> false under the protection of the
  // mutex.  if it is already false, we need not retake the mutex at
  // all.
  if (!need_addr)
    return;

  sh_mtx.Lock();
  if (need_addr) {
    entity_addr_t t = peer_addr_for_me;
    if (my_inst.addr.get_port())  // messenger may not know its port
      t.set_port(my_inst.addr.get_port());
    my_inst.addr.addr = t.addr;
    ldout(cct,2) << "learned my addr " << my_inst.addr << dendl;
    need_addr = false;
    // init_local_connection();
  }
  sh_mtx.Unlock();

}

int XioMessenger::new_session(struct xio_session *session,
			      struct xio_new_session_req *req,
			      void *cb_user_context)
{
  if (shutdown_called.read()) {
    return xio_reject(
      session, XIO_E_SESSION_REFUSED, NULL /* udata */, 0 /* udata len */);
  }
  int code = portals.accept(session, req, cb_user_context);
  if (! code)
    nsessions.inc();
  return code;
} /* new_session */

int XioMessenger::session_event(struct xio_session *session,
				struct xio_session_event_data *event_data,
				void *cb_user_context)
{
  XioConnection *xcon;
  bool do_collection = cct->_conf->ms_status_collection;

  switch (event_data->event) {
  case XIO_SESSION_CONNECTION_ESTABLISHED_EVENT:
  {
    struct xio_connection *conn = event_data->conn;
    struct xio_connection_attr xcona;
    entity_addr_t peer_addr_for_me, paddr;

    xcon = static_cast<XioConnection*>(event_data->conn_user_context);

    ldout(cct,2) << "connection established " << event_data->conn
      << " session " << session << " xcon " << xcon << dendl;

    (void) xio_query_connection(conn, &xcona,
				XIO_CONNECTION_ATTR_LOCAL_ADDR|
				XIO_CONNECTION_ATTR_PEER_ADDR);
    (void) entity_addr_from_sockaddr(&peer_addr_for_me, (struct sockaddr *) &xcona.local_addr);
    (void) entity_addr_from_sockaddr(&paddr, (struct sockaddr *) &xcona.peer_addr);
    //set_myaddr(peer_addr_for_me);
    learned_addr(peer_addr_for_me);
    ldout(cct,2) << "client: connected from " << peer_addr_for_me << " to " << paddr << dendl;

    /* notify hook */
    this->ms_deliver_handle_connect(xcon);
    this->ms_deliver_handle_fast_connect(xcon);
  }
  break;

  case XIO_SESSION_NEW_CONNECTION_EVENT:
  {
    struct xio_connection *conn = event_data->conn;
    struct xio_connection_attr xcona;
    entity_inst_t s_inst;
    entity_addr_t peer_addr_for_me;

    (void) xio_query_connection(conn, &xcona,
				XIO_CONNECTION_ATTR_CTX|
				XIO_CONNECTION_ATTR_PEER_ADDR|
				XIO_CONNECTION_ATTR_LOCAL_ADDR);
    /* XXX assumes RDMA */
    (void) entity_addr_from_sockaddr(&s_inst.addr,
				     (struct sockaddr *) &xcona.peer_addr);
    (void) entity_addr_from_sockaddr(&peer_addr_for_me, (struct sockaddr *) &xcona.local_addr);

    xcon = new XioConnection(this, XioConnection::PASSIVE, s_inst);
    xcon->session = session;

    if (do_collection)
      cct->get_msstatus_collection()->inc_conns(get_mname());

    struct xio_context_attr xctxa;
    (void) xio_query_context(xcona.ctx, &xctxa, XIO_CONTEXT_ATTR_USER_CTX);

    xcon->conn = conn;
    xcon->portal = static_cast<XioPortal*>(xctxa.user_context);
    assert(xcon->portal);

    xcona.user_context = xcon;
    (void) xio_modify_connection(conn, &xcona, XIO_CONNECTION_ATTR_USER_CTX);

    xcon->connected.set(true);

    /* sentinel ref */
    xcon->get(); /* xcon->nref == 1 */
    conns_sp.lock();
    conns_list.push_back(*xcon);
    /* XXX we can't put xcon in conns_entity_map becase we don't yet know
     * it's peer address */
    conns_sp.unlock();

    /* XXXX pre-merge of session startup negotiation ONLY! */
    xcon->cstate.state_up_ready(XioConnection::CState::OP_FLAG_NONE);

    ldout(cct,2) << "New connection session " << session
      << " xcon " << xcon << " on msgr: " << this << " portal: " << xcon->portal << dendl;
    ldout(cct,2) << "Server: connected from " << s_inst.addr << " to " << peer_addr_for_me << dendl;
  }
  break;
  case XIO_SESSION_CONNECTION_ERROR_EVENT:
  case XIO_SESSION_CONNECTION_CLOSED_EVENT: /* orderly discon */
  case XIO_SESSION_CONNECTION_DISCONNECTED_EVENT: /* unexpected discon */
  case XIO_SESSION_CONNECTION_REFUSED_EVENT:
    xcon = static_cast<XioConnection*>(event_data->conn_user_context);
    ldout(cct,2) << xio_session_event_types[event_data->event]
      << " xcon " << xcon << " session " << session  << dendl;
    if (likely(!!xcon)) {
      Spinlock::Locker lckr(conns_sp);
      XioConnection::EntitySet::iterator conn_iter =
	conns_entity_map.find(xcon->peer, XioConnection::EntityComp());
      if (conn_iter != conns_entity_map.end()) {
	XioConnection *xcon2 = &(*conn_iter);
	if (xcon == xcon2) {
	  conns_entity_map.erase(conn_iter);
	}
      }
      /* check if citer on conn_list */
      if (xcon->conns_hook.is_linked()) {
        /* now find xcon on conns_list and erase */
        XioConnection::ConnList::iterator citer =
            XioConnection::ConnList::s_iterator_to(*xcon);
        conns_list.erase(citer);
      }

      if (do_collection)
        cct->get_msstatus_collection()->dec_conns(get_mname());

      xcon->on_disconnect_event();
      xcon->cstate.state_discon();
      event_finisher.queue(new MsResetContext(xcon));
    }
    break;
  case XIO_SESSION_CONNECTION_TEARDOWN_EVENT:
    xio_connection_destroy(event_data->conn);
    xcon = static_cast<XioConnection*>(event_data->conn_user_context);
    ldout(cct,2) << xio_session_event_types[event_data->event]
      << " xcon " << xcon << " session " << session << dendl;
    xcon->on_teardown_event();
    break;
  case XIO_SESSION_TEARDOWN_EVENT:
    xio_session_destroy(session);
    ldout(cct,2) << "xio_session_teardown " << session << dendl;
    if (unlikely(XioPool::trace_mempool)) {
      xp_stats.dump("xio session dtor", reinterpret_cast<uint64_t>(session));
    }
    if (nsessions.dec() == 0) {
      Mutex::Locker lck(sh_mtx);
      if (nsessions.read() == 0)
	sh_cond.Signal();
    }
    break;
  default:
    break;
  };

  return 0;
}

enum bl_type
{
  BUFFER_PAYLOAD,
  BUFFER_MIDDLE,
  BUFFER_DATA
};

#define MAX_XIO_BUF_SIZE 1044480

static inline int
xio_count_buffers(buffer::list& bl, int& req_size, int& msg_off, int& req_off)
{

  const std::list<buffer::ptr>& buffers = bl.buffers();
  list<bufferptr>::const_iterator pb;
  size_t size, off;
  int result;
  int first = 1;

  off = size = 0;
  result = 0;
  for (;;) {
    if (off >= size) {
      if (first) pb = buffers.begin(); else ++pb;
      if (pb == buffers.end()) {
	break;
      }
      off = 0;
      size = pb->length();
      first = 0;
    }
    size_t count = size - off;
    if (!count) continue;
    if (req_size + count > MAX_XIO_BUF_SIZE) {
	count = MAX_XIO_BUF_SIZE - req_size;
    }

    ++result;

    /* advance iov and perhaps request */

    off += count;
    req_size += count;
    ++msg_off;
    if (unlikely(msg_off >= XIO_MSGR_IOVLEN || req_size >= MAX_XIO_BUF_SIZE)) {
      ++req_off;
      msg_off = 0;
      req_size = 0;
    }
  }

  return result;
}

static inline void
xio_place_buffers(buffer::list& bl, XioMsg *xmsg, struct xio_msg*& req,
		  struct xio_iovec_ex*& msg_iov, int& req_size,
		  int ex_cnt, int& msg_off, int& req_off, bl_type type)
{

  const std::list<buffer::ptr>& buffers = bl.buffers();
  list<bufferptr>::const_iterator pb;
  struct xio_iovec_ex* iov;
  size_t size, off;
  const char *data = NULL;
  int first = 1;

  off = size = 0;
  for (;;) {
    if (off >= size) {
      if (first) pb = buffers.begin(); else ++pb;
      if (pb == buffers.end()) {
	break;
      }
      off = 0;
      size = pb->length();
      data = pb->c_str();	 // is c_str() efficient?
      first = 0;
    }
    size_t count = size - off;
    if (!count) continue;
    if (req_size + count > MAX_XIO_BUF_SIZE) {
	count = MAX_XIO_BUF_SIZE - req_size;
    }

    /* assign buffer */
    iov = &msg_iov[msg_off];
    iov->iov_base = (void *) (&data[off]);
    iov->iov_len = count;

    switch (type) {
    case BUFFER_DATA:
      //break;
    default:
    {
      struct xio_reg_mem *mp = get_xio_mp(*pb);
      iov->mr = (mp) ? mp->mr : NULL;
    }
      break;
    }

    /* advance iov(s) */

    off += count;
    req_size += count;
    ++msg_off;

    /* next request if necessary */

    if (unlikely(msg_off >= XIO_MSGR_IOVLEN || req_size >= MAX_XIO_BUF_SIZE)) {
      /* finish this request */
      req->out.pdata_iov.nents = msg_off;
      /* advance to next, and write in it if it's not the last one. */
      if (++req_off >= ex_cnt) {
	req = 0;	/* poison.  trap if we try to use it. */
	msg_iov = NULL;
      } else {
	req = &xmsg->req_arr[req_off].msg;
	msg_iov = req->out.pdata_iov.sglist;
      }
      msg_off = 0;
      req_size = 0;
    }
  }
}

int XioMessenger::bind(const entity_addr_t& addr)
{
  const entity_addr_t *a = &addr;
  struct entity_addr_t _addr = *a;

  if (a->is_blank_ip()) {
    a = &_addr;
    std::vector <std::string> my_sections;
    cct->_conf->get_my_sections(my_sections);
    std::string rdma_local_str;
    if (cct->_conf->get_val_from_conf_file(my_sections, "rdma local",
				      rdma_local_str, true) == 0) {
      struct entity_addr_t local_rdma_addr;
      local_rdma_addr = *a;
      const char *ep;
      if (!local_rdma_addr.parse(rdma_local_str.c_str(), &ep)) {
	ldout(cct,0) << "ERROR:  Cannot parse rdma local: " << rdma_local_str << dendl;
	return -EINVAL;
      }
      if (*ep) {
	ldout(cct,0) << "WARNING: 'rdma local trailing garbage ignored: '" << ep << dendl;
      }
      ldout(cct, 2) << "Found rdma_local address " << rdma_local_str.c_str() << dendl;
      int p = _addr.get_port();
      _addr.set_sockaddr(reinterpret_cast<struct sockaddr *>(
			  &local_rdma_addr.ss_addr()));
      _addr.set_port(p);
    } else {
      ldout(cct,0) << "WARNING: need 'rdma local' config for remote use!" <<dendl;
    }
  }

  entity_addr_t shift_addr = *a;

  string base_uri = xio_uri_from_entity(cct->_conf->xio_transport_type,
					shift_addr, false /* want_port */);
  ldout(cct,4) << "XioMessenger " << this << " bind: xio_uri "
    << base_uri << ':' << shift_addr.get_port() << dendl;

  uint16_t port0;
  int r = portals.bind(&xio_msgr_ops, base_uri, shift_addr.get_port(), &port0);
  if (r == 0) {
    shift_addr.set_port(port0);
    shift_addr.nonce = nonce;
    set_myaddr(shift_addr);
    did_bind = true;
  }
  return r;
} /* bind */

int XioMessenger::rebind(const set<int>& avoid_ports)
{
  ldout(cct,4) << "XioMessenger " << this << " rebind attempt" << dendl;
  return 0;
} /* rebind */

int XioMessenger::start()
{
  portals.start();
  dispatch_strategy->start();
  event_finisher.start();
  if (!did_bind) {
	  my_inst.addr.nonce = nonce;
  }
  started = true;
  return 0;
}

void XioMessenger::wait()
{
  portals.join();
  dispatch_strategy->wait();
} /* wait */

int XioMessenger::_send_message(Message *m, const entity_inst_t& dest)
{
  ConnectionRef conn = get_connection(dest);
  if (conn)
    return _send_message(m, &(*conn));
  else
    return EINVAL;
} /* send_message(Message *, const entity_inst_t&) */

static inline XioMsg* pool_alloc_xio_msg(Message *m, XioConnection *xcon,
  int ex_cnt)
{
  struct xio_reg_mem mp_mem;
  int e = xpool_alloc(xio_msgr_noreg_mpool, sizeof(XioMsg), &mp_mem);
  if (!!e)
    return NULL;
  XioMsg *xmsg = reinterpret_cast<XioMsg*>(mp_mem.addr);
  assert(!!xmsg);
  new (xmsg) XioMsg(m, xcon, mp_mem, ex_cnt);
  return xmsg;
}

int XioMessenger::_send_message(Message *m, Connection *con)
{
  if (con == loop_con.get() /* intrusive_ptr get() */) {
    ldout(cct,2) << "send my self msg-1 m[" << m << "] conn[" << con << "]" << dendl;
    m->set_connection(con);
    m->set_src(get_myinst().name);
    m->set_seq(loop_con->next_seq());
    ds_dispatch(m);
    return 0;
  }

  XioConnection *xcon = static_cast<XioConnection*>(con);

  /* If con is not in READY state, we have to enforce policy */
  if (0 && xcon->cstate.session_state.read() != XioConnection::UP) {
    pthread_spin_lock(&xcon->sp);
    if (xcon->cstate.session_state.read() != XioConnection::UP) {
      xcon->outgoing.mqueue.push_back(*m);
      pthread_spin_unlock(&xcon->sp);
      return -1;
    }
    pthread_spin_unlock(&xcon->sp);
  }

  return _send_message_impl(m, xcon);
} /* send_message(Message* m, Connection *con) */

int XioMessenger::_send_message_impl(Message* m, XioConnection* xcon)
{
  int code = 0;

  Mutex::Locker l(xcon->lock);
  if (unlikely(XioPool::trace_mempool)) {
    static uint32_t nreqs;
    if (unlikely((++nreqs % 65536) == 0)) {
      xp_stats.dump(__func__, nreqs);
    }
  }

  m->set_seq(xcon->state.next_out_seq());
  m->set_magic(magic); // trace flags and special handling

  m->encode(xcon->get_features(), this->crcflags);

  buffer::list &payload = m->get_payload();
  buffer::list &middle = m->get_middle();
  buffer::list &data = m->get_data();

  int msg_off = 0;
  int req_off = 0;
  int req_size = 0;
  int nbuffers =
    xio_count_buffers(payload, req_size, msg_off, req_off) +
    xio_count_buffers(middle, req_size, msg_off, req_off) +
    xio_count_buffers(data, req_size, msg_off, req_off);

  int ex_cnt = req_off;
  if (msg_off == 0 && ex_cnt > 0) {
    // no buffers for last msg
    ldout(cct,10) << "msg_off 0, ex_cnt " << ex_cnt << " -> " << ex_cnt-1 << dendl;
    ex_cnt--;
  }

  /* get an XioMsg frame */
  XioMsg *xmsg = pool_alloc_xio_msg(m, xcon, ex_cnt);
  if (! xmsg) {
    /* could happen if Accelio has been shutdown */
    ldout(cct,0) << __func__ << " malloc error" << xmsg << dendl;  
    return ENOMEM;
  }

  ldout(cct,4) << __func__ << " " << m << " new XioMsg " << xmsg
       << " req_0 " << &xmsg->req_0.msg << " msg type " << m->get_type()
       << " features: " << xcon->get_features()
       << " conn " << xcon->conn << " sess " << xcon->session << dendl;

  if (magic & (MSG_MAGIC_XIO)) {

    /* XXXX verify */
    switch (m->get_type()) {
    case 43:
    // case 15:
      ldout(cct,4) << __func__ << "stop 43 " << m->get_type() << " " << *m << dendl;
      buffer::list &payload = m->get_payload();
      ldout(cct,4) << __func__ << "payload dump:" << dendl;
      payload.hexdump(cout);
    }
  }

  struct xio_msg *req = &xmsg->req_0.msg;
  struct xio_iovec_ex *msg_iov = req->out.pdata_iov.sglist;

  if (magic & (MSG_MAGIC_XIO)) {
    ldout(cct,4) << "payload: " << payload.buffers().size() <<
      " middle: " << middle.buffers().size() <<
      " data: " << data.buffers().size() <<
      dendl;
  }

  if (unlikely(ex_cnt > 0)) {
    ldout(cct,4) << __func__ << " buffer cnt > XIO_MSGR_IOVLEN (" <<
      ((XIO_MSGR_IOVLEN-1) + nbuffers) << ")" << dendl;
  }

  /* do the invariant part */
  msg_off = 0;
  req_off = -1; /* most often, not used */
  req_size = 0;

  xio_place_buffers(payload, xmsg, req, msg_iov, req_size, ex_cnt, msg_off,
		    req_off, BUFFER_PAYLOAD);

  xio_place_buffers(middle, xmsg, req, msg_iov, req_size, ex_cnt, msg_off,
		    req_off, BUFFER_MIDDLE);

  xio_place_buffers(data, xmsg, req, msg_iov, req_size, ex_cnt, msg_off,
		    req_off, BUFFER_DATA);
  ldout(cct,10) << "ex_cnt " << ex_cnt << ", req_off " << req_off
    << ", msg_cnt " << xmsg->hdr.msg_cnt << dendl;

  /* finalize request */
  if (msg_off)
    req->out.pdata_iov.nents = msg_off;

  /* fixup first msg */
  req = &xmsg->req_0.msg;

  const std::list<buffer::ptr>& header = xmsg->hdr.get_bl().buffers();
  assert(header.size() == 1); /* XXX */
  list<bufferptr>::const_iterator pb = header.begin();
  req->out.header.iov_base = (char*) pb->c_str();
  req->out.header.iov_len = pb->length();

  /* deliver via xio, preserve ordering */
  if (xmsg->hdr.msg_cnt > 1) {
    struct xio_msg *head = &xmsg->req_0.msg;
    struct xio_msg *tail = head;
    for (req_off = 0; ((unsigned) req_off) < xmsg->hdr.msg_cnt-1; ++req_off) {
	req = &xmsg->req_arr[req_off].msg;
	assert(!req->in.pdata_iov.nents);
	assert(req->out.pdata_iov.nents || !nbuffers);
	tail->next = req;
	tail = req;
     }
    tail->next = NULL;
  }

  if (xcon->cstate.session_state.read() == XioConnection::DISCONNECTED || !xcon->is_connected()) {
      ldout(cct,4) << "warning: conn disconnected.conn[" << xcon << "]" << dendl;
      xmsg->put();
      return -2;
  }

  if(xcon->cstate.session_state.read() == XioConnection::FLOW_CONTROLLED)
  {
      xcon->cstate.state_up_ready(XioConnection::CState::OP_FLAG_LOCKED);
      code = -3;
  }

  xcon->portal->enqueue_for_send(xcon, xmsg);

  return code;
} /* send_message(Message *, Connection *) */

int XioMessenger::shutdown()
{
  shutdown_called.set(true);
  portals.shutdowncalled();
  conns_sp.lock();
  XioConnection::ConnList::iterator iter;
  iter = conns_list.begin();
  for (iter = conns_list.begin(); iter != conns_list.end(); ++iter) {
    (void) iter->disconnect(); // XXX mark down?
  }
  conns_sp.unlock();
  while(nsessions.read() > 0) {
    Mutex::Locker lck(sh_mtx);
    if (nsessions.read() > 0)
      sh_cond.Wait(sh_mtx);
  }
  portals.shutdown();
  dispatch_strategy->shutdown();
  event_finisher.stop();
  did_bind = false;
  started = false;
  return 0;
} /* shutdown */

ConnectionRef XioMessenger::get_connection(const entity_inst_t& dest)
{
  if (shutdown_called.read())
    return NULL;

  const entity_inst_t& self_inst = get_myinst();
  if ((&dest == &self_inst) ||
      (dest == self_inst)) {
    return get_loopback_connection();
  }

  Mutex::Locker l(gc_mtx);
  conns_sp.lock();
  XioConnection::EntitySet::iterator conn_iter =
    conns_entity_map.find(dest, XioConnection::EntityComp());
  if (conn_iter != conns_entity_map.end()) {
    ConnectionRef cref = &(*conn_iter);
    conns_sp.unlock();
    return cref;
  }
  else {
    conns_sp.unlock();
    string xio_uri = xio_uri_from_entity(cct->_conf->xio_transport_type,
					 dest.addr, true /* want_port */);

    ldout(cct,4) << "XioMessenger " << this << " get_connection: xio_uri "
      << xio_uri << dendl;

    /* XXX client session creation parameters */
    struct xio_session_params params = {};
    params.type             = XIO_SESSION_CLIENT;
    params.initial_sn       = 0;
    params.ses_ops          = &xio_msgr_ops;
    params.user_context     = this;
    params.private_data     = NULL;
    params.private_data_len = 0;
    params.uri              = xio_uri.c_str();

    XioConnection *xcon = new XioConnection(this, XioConnection::ACTIVE,
					    dest);

    xcon->session = xio_session_create(&params);
    if (! xcon->session) {
      delete xcon;
      return NULL;
    }
    nsessions.inc();

    /* this should cause callbacks with user context of conn, but
     * we can always set it explicitly */
    struct xio_connection_params xcp = {};
    xcp.session           = xcon->session;
    xcp.ctx               = xcon->portal->ctx,
    xcp.conn_idx          = 0; /* XXX auto_count */
    xcp.enable_tos        = 0;
    xcp.tos               = 0;
    xcp.out_addr          = NULL;
    xcp.conn_user_context = xcon;
    xcon->conn = xio_connect(&xcp);
    if (!xcon->conn) {
	    xio_session_destroy(xcon->session);
	    delete xcon;
	    ldout(cct,2) << "error: new connection failed: uri=" << xio_uri << dendl;
	    return NULL;
    }
    xcon->connected.set(true);

    /* sentinel ref */
    xcon->get(); /* xcon->nref == 1 */
    conns_sp.lock();
    conns_list.push_back(*xcon);
    conns_entity_map.insert(*xcon);
    if (cct->_conf->ms_status_collection)
      cct->get_msstatus_collection()->inc_conns(get_mname());
    conns_sp.unlock();

    /* XXXX pre-merge of session startup negotiation ONLY! */
    xcon->cstate.state_up_ready(XioConnection::CState::OP_FLAG_NONE);

    ldout(cct,2) << "New connection xcon: " << xcon <<
      " up_ready on session " << xcon->session <<
      " on msgr: " << this << " portal: " << xcon->portal << dendl;

    return xcon->get(); /* nref +1 */
  }
} /* get_connection */

ConnectionRef XioMessenger::get_loopback_connection()
{
  return (loop_con.get());
} /* get_loopback_connection */

void XioMessenger::mark_down(const entity_addr_t& addr)
{
  entity_inst_t inst(entity_name_t(), addr);
  Spinlock::Locker lckr(conns_sp);
  XioConnection::EntitySet::iterator conn_iter =
    conns_entity_map.find(inst, XioConnection::EntityComp());
  if (conn_iter != conns_entity_map.end()) {
      (*conn_iter)._mark_down(XioConnection::CState::OP_FLAG_NONE);
    }
} /* mark_down(const entity_addr_t& */

void XioMessenger::mark_down(Connection* con)
{
  XioConnection *xcon = static_cast<XioConnection*>(con);
  xcon->_mark_down(XioConnection::CState::OP_FLAG_NONE);
} /* mark_down(Connection*) */

void XioMessenger::mark_down_all()
{
  Spinlock::Locker lckr(conns_sp);
  XioConnection::EntitySet::iterator conn_iter;
  for (conn_iter = conns_entity_map.begin(); conn_iter !=
	 conns_entity_map.begin(); ++conn_iter) {
    (*conn_iter)._mark_down(XioConnection::CState::OP_FLAG_NONE);
  }
} /* mark_down_all */

static inline XioMarkDownHook* pool_alloc_markdown_hook(
  XioConnection *xcon, Message *m)
{
  struct xio_reg_mem mp_mem;
  int e = xio_mempool_alloc(xio_msgr_noreg_mpool,
			    sizeof(XioMarkDownHook), &mp_mem);
  if (!!e)
    return NULL;
  XioMarkDownHook *hook = static_cast<XioMarkDownHook*>(mp_mem.addr);
  new (hook) XioMarkDownHook(xcon, m, mp_mem);
  return hook;
}

void XioMessenger::mark_down_on_empty(Connection* con)
{
  XioConnection *xcon = static_cast<XioConnection*>(con);
  MNop* m = new MNop();
  m->tag = XIO_NOP_TAG_MARKDOWN;
  m->set_completion_hook(pool_alloc_markdown_hook(xcon, m));
  // stall new messages
  xcon->cstate.session_state.set(XioConnection::BARRIER);
  (void) _send_message_impl(m, xcon);
}

void XioMessenger::mark_disposable(Connection *con)
{
  XioConnection *xcon = static_cast<XioConnection*>(con);
  xcon->_mark_disposable(XioConnection::CState::OP_FLAG_NONE);
}

void XioMessenger::try_insert(XioConnection *xcon)
{
  Spinlock::Locker lckr(conns_sp);
  /* already resident in conns_list */
  conns_entity_map.insert(*xcon);
}

XioMessenger::~XioMessenger()
{
  delete dispatch_strategy;
  nInstances.dec();
} /* dtor */



//-----------------sff
QueueParser::QueueParser(int n_threads)
{
    this->n_threads = n_threads;
    this->stop = false;
	iindex = 0;
}

QueueParser::ParserThread::ParserThread(QueueParser *dq, int inum) 
{ 
    queueparser = dq;
    itasknum = 0;
	inum = inum;

    pthread_cond_init(&m_hCond,NULL);
    pthread_mutex_init(&m_hMutex,NULL);
    m_pstrulisttailer = m_pstrulistheader = (link_list*)malloc(sizeof(link_list));
    m_pstrulistheader->ptr = NULL;
    m_pstrulistheader->next = NULL;
    bExite = false;
}

QueueParser::~QueueParser() 
{
    ParserThread *thrd;
    for (int ix = 0; ix < n_threads; ++ix) {
        thrd  = parser_threads[ix];

        delete thrd;
        thrd = NULL;
        parser_threads[ix] = NULL;
    }
}

void QueueParser::start()
{
    ParserThread *thrd;
    for (int ix = 0; ix < n_threads; ++ix) {
        thrd = new ParserThread(this, ix);
        thrd->create();
        parser_threads[ix] = thrd;
    }
}

void QueueParser::wait()
{
    ParserThread *thrd;
    for (int ix = 0; ix < n_threads; ++ix) {
        thrd  = parser_threads[ix];
        thrd->join();
    }
}

void QueueParser::shutdown()
{
    ParserThread *thrd;
    stop = true;

    for (int ix = 0; ix < n_threads; ++ix) {
        thrd  = parser_threads[ix];
        thrd->bExite = false;
        pthread_cond_signal(&thrd->m_hCond);
    }
}

int QueueParser::ParserThread::Push(PROTOCOL_PARSER_STRU_T* ptaskinfo)
{
    link_list *l = (link_list *)malloc(sizeof(link_list));
    l->next = NULL;
    l->ptr = ptaskinfo;
    bool bSeted = false;

    pthread_mutex_lock(&m_hMutex);
    if(NULL == m_pstrulistheader->next)
        bSeted = true;

    m_pstrulisttailer->next = l;
    m_pstrulisttailer = l;
    pthread_mutex_unlock(&m_hMutex);
    if(bSeted)
    {
        pthread_cond_signal(&m_hCond);
    }

    //
    itasknum += 1;
	//printf("input the new infor, index[%d] tasknum[%d]\n", inum, itasknum);

    return itasknum;
}

int QueueParser::Push(PROTOCOL_PARSER_STRU_T* ptaskinfo)
{
		ParserThread *thrd;
		int iret = 0;
		for (;1;) {
				thrd  = parser_threads[iindex];
				iret = thrd->Push(ptaskinfo);
				iindex++;
				if(iindex >= n_threads)
						iindex = 0;
				break;
		}

		return iret;
}

//queue parse implemetation
void* QueueParser::ParserThread::entry()
{
    link_list *l = NULL;
	int iinum = 0;
	link_list *ltemp = NULL;
	XioConnection* xcon;
	struct xio_session *session;
	struct xio_msg *req;
	int more_in_batch;
	void *cb_user_context;
	XioDispatchHook *m_hook;
	
	while(!bExite)
    {
        PROTOCOL_PARSER_STRU_T* pstrutaskinfo = NULL;
        pthread_mutex_lock(&m_hMutex);
        while(NULL == m_pstrulistheader->next)
        {
            pthread_cond_wait(&m_hCond,&m_hMutex);
        }

        l = m_pstrulistheader->next;
        m_pstrulisttailer = m_pstrulistheader;
        m_pstrulistheader->ptr = NULL;
        m_pstrulistheader->next = NULL;
		itasknum= 0;
        pthread_mutex_unlock(&m_hMutex);

		iinum = 0;
        while (NULL != l)
        {
		    pstrutaskinfo = (PROTOCOL_PARSER_STRU_T*)l->ptr;
		    xcon = pstrutaskinfo->xcon;
		    m_hook = pstrutaskinfo->m_hook;
		    xcon->on_msg_req2(m_hook);
			//printf("entry task threadnum[%d] index[%d] conn[%p]session[%p]hook[%p]\n", inum, iinum++, xcon, session, m_hook);

			ltemp = l;
            l = l->next;

            safe_free(pstrutaskinfo);
            
			ltemp->ptr = NULL;
            ltemp->next = NULL;
            safe_free(ltemp);
        }
    }
}
