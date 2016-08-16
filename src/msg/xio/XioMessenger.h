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

#ifndef XIO_MESSENGER_H
#define XIO_MESSENGER_H

#include "msg/SimplePolicyMessenger.h"
extern "C" {
#include "libxio.h"
}
#include "XioConnection.h"
#include "XioPortal.h"
#include "QueueStrategy.h"
#include "include/atomic.h"
#include "common/Thread.h"
#include "common/Mutex.h"
#include "common/Finisher.h"
#include "include/Spinlock.h"

class XioInit {
  /* safe to be called multiple times */
  void package_init(CephContext *cct);

protected:
  XioInit(CephContext *cct) {
    this->package_init(cct);
  }
};

/**
*/
typedef struct _PROTOCOL_PARSER_STRU_T
{
    XioConnection* xcon;
	XioDispatchHook *m_hook;
    _PROTOCOL_PARSER_STRU_T()
    {
		xcon = NULL;
		m_hook = NULL;
    }

    _PROTOCOL_PARSER_STRU_T ( const _PROTOCOL_PARSER_STRU_T & struprotocolparser )
    {
		xcon = struprotocolparser.xcon;
		m_hook = struprotocolparser.m_hook;
    }
}PROTOCOL_PARSER_STRU_T;

typedef struct _link_list {
    void *ptr;
    struct _link_list *next;
}link_list;

class QueueParser : public DispatchStrategy {
    int n_threads;
    bool stop;
    int iindex;

public:
    class ParserThread : public Thread {
    public:
        link_list* m_pstrulistheader;
        link_list* m_pstrulisttailer;
        pthread_cond_t m_hCond;
        pthread_mutex_t m_hMutex;
        QueueParser* queueparser;
        bool bExite;
        int itasknum;
        int inum;

        explicit ParserThread(QueueParser *dq, int inum);
        int Push(PROTOCOL_PARSER_STRU_T* ptaskinfo);
        void* entry();
    };

    ParserThread* parser_threads[10];
public:
    explicit QueueParser(int n_threads);
	int Push(PROTOCOL_PARSER_STRU_T* ptaskinfo);
    virtual void ds_dispatch(Message *m){};
    virtual void shutdown();
    virtual void start();
    virtual void wait();
    virtual ~QueueParser();
};

class XioMessenger : public SimplePolicyMessenger, XioInit
{
public:
  QueueParser* queueparser;
private:
  static atomic_t nInstances;
  atomic_t nsessions;
  atomic_t shutdown_called;
  Spinlock conns_sp;
  XioConnection::ConnList conns_list;
  XioConnection::EntitySet conns_entity_map;
  XioPortals portals;
  DispatchStrategy* dispatch_strategy;
  XioLoopbackConnectionRef loop_con;
  uint32_t special_handling;
  Mutex sh_mtx;
  Mutex gc_mtx;
  Cond sh_cond;
  bool need_addr;
  bool did_bind;

  /// approximately unique ID set by the Constructor for use in entity_addr_t
  uint64_t nonce;

  friend class XioConnection;

public:
  XioMessenger(CephContext *cct, entity_name_t name,
	       string mname, uint64_t nonce, uint64_t features,
	       DispatchStrategy* ds = new QueueStrategy(1));

  virtual ~XioMessenger();

  XioPortal* get_portal() { return portals.get_next_portal(); }

  virtual void set_myaddr(const entity_addr_t& a) {
    Messenger::set_myaddr(a);
    loop_con->set_peer_addr(a);
  }

  int _send_message(Message *m, const entity_inst_t &dest);
  int _send_message(Message *m, Connection *con);
  int _send_message_impl(Message *m, XioConnection *xcon);

  uint32_t get_magic() { return magic; }
  void set_magic(int _magic) { magic = _magic; }
  uint32_t get_special_handling() { return special_handling; }
  void set_special_handling(int n) { special_handling = n; }
  int pool_hint(uint32_t size);
  void try_insert(XioConnection *xcon);

  /* xio hooks */
  int new_session(struct xio_session *session,
		  struct xio_new_session_req *req,
		  void *cb_user_context);

  int session_event(struct xio_session *session,
		    struct xio_session_event_data *event_data,
		    void *cb_user_context);

  /* Messenger interface */
  virtual void set_addr_unknowns(entity_addr_t &addr)
    { } /* XXX applicable? */

  virtual int get_dispatch_queue_len()
    { return 0; } /* XXX bogus? */

  virtual double get_dispatch_queue_max_age(utime_t now)
    { return 0; } /* XXX bogus? */

  virtual void set_cluster_protocol(int p)
    { }

  virtual int bind(const entity_addr_t& addr);

  virtual int rebind(const set<int>& avoid_ports);

  virtual int start();

  virtual void wait();

  virtual int shutdown();

  virtual int send_message(Message *m, const entity_inst_t &dest) {
    return _send_message(m, dest);
  }

  virtual int lazy_send_message(Message *m, const entity_inst_t& dest)
    { return EINVAL; }

  virtual int lazy_send_message(Message *m, Connection *con)
    { return EINVAL; }

  virtual ConnectionRef get_connection(const entity_inst_t& dest);

  virtual ConnectionRef get_loopback_connection();

  virtual int send_keepalive(const entity_inst_t& dest)
    { return EINVAL; }

  virtual int send_keepalive(Connection *con)
    { return EINVAL; }

  virtual void mark_down(const entity_addr_t& a);
  virtual void mark_down(Connection *con);
  virtual void mark_down_all();
  virtual void mark_down_on_empty(Connection *con);
  virtual void mark_disposable(Connection *con);

  void ds_dispatch(Message *m)
    { dispatch_strategy->ds_dispatch(m); }

  /**
   * Tell the XioMessenger its full IP address.
   *
   * This is used by clients when connecting to other endpoints, and
   * probably shouldn't be called by anybody else.
   */
  void learned_addr(const entity_addr_t& peer_addr_for_me);

private:
  int get_nconns_per_portal(const string &mname);
  int get_nportals(const string &mname);

protected:
  virtual void ready()
    { }

public:
  uint64_t local_features;
  Finisher event_finisher;

  Connection *create_anon_connection() {
    entity_inst_t inst;
    return new XioConnection(NULL, XioConnection::ACTIVE, inst);
  }

};

class MsResetContext: public Context {
private:
  XioConnection *xcon;

public:
  MsResetContext(XioConnection *xcon) : xcon(xcon) {
    xcon->get();
  }

  void finish(int r) {
    XioMessenger* msgr = static_cast<XioMessenger*>(xcon->get_messenger());
    msgr->ms_deliver_handle_reset(xcon);
    xcon->put();
  }
};

#endif /* XIO_MESSENGER_H */
