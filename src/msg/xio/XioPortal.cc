// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 CohortFS, LLC
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "XioPortal.h"
#include "XioMessenger.h"
#include <stdio.h>
#include "common/ms_status.h"

#define dout_subsys ceph_subsys_xio

int XioPortal::bind(struct xio_session_ops *ops, const string &base_uri,
		    uint16_t port, uint16_t *assigned_port)
{
  // format uri
  char buf[40];
  xio_uri = base_uri;
  xio_uri += ":";
  sprintf(buf, "%d", port);
  xio_uri += buf;

  uint16_t assigned;
  server = xio_bind(ctx, ops, xio_uri.c_str(), &assigned, 0, msgr);
  if (server == NULL)
    return xio_errno();

  // update uri if port changed
  if (port != assigned) {
    xio_uri = base_uri;
    xio_uri += ":";
    sprintf(buf, "%d", assigned);
    xio_uri += buf;
  }

  portal_id = const_cast<char*>(xio_uri.c_str());
  if (assigned_port)
    *assigned_port = assigned;
  ldout(msgr->cct,20) << "xio_bind: portal " << xio_uri
    << " returned server " << server << dendl;
  return 0;
}

int XioPortals::bind(struct xio_session_ops *ops, const string& base_uri,
		     uint16_t port, uint16_t *port0)
{
  /* a server needs at least 1 portal */
  if (n < 1)
    return EINVAL;
  Messenger *msgr = portals[0]->msgr;
  portals.resize(n);

  uint16_t port_min = msgr->cct->_conf->ms_bind_port_min;
  const uint16_t port_max = msgr->cct->_conf->ms_bind_port_max;

  /* bind the portals */
  for (size_t i = 0; i < portals.size(); i++) {
    uint16_t result_port;
    if (port != 0) {
      // bind directly to the given port
      int r = portals[i]->bind(ops, base_uri, port, &result_port);
      if (r != 0)
        return -r;
    } else {
      int r = EADDRINUSE;
      // try ports within the configured range
      for (; port_min <= port_max; port_min++) {
        r = portals[i]->bind(ops, base_uri, port_min, &result_port);
        if (r == 0)
          break;
      }
      if (r != 0) {
        lderr(msgr->cct) << "portal.bind unable to bind to " << base_uri
            << " on any port in range " << msgr->cct->_conf->ms_bind_port_min
            << "-" << port_max << ": " << xio_strerror(r) << dendl;
        return -r;
      }
    }

    ldout(msgr->cct,5) << "xp::bind: portal " << i << " bind OK: "
      << portals[i]->xio_uri << dendl;

    if (i == 0 && port0 != NULL)
      *port0 = result_port;
    port = result_port + 1; // use port 0 for all subsequent portals
  }

  return 0;
}

void XioPortal::requeue_all_xcon(XioConnection* xcon,
                        XioSubmit::Queue::iterator& q_iter,
                        XioSubmit::Queue& send_q) {
    // XXX gather all already-dequeued outgoing messages for xcon
    // and push them in FIFO order to front of the input queue,
    // and mark the connection as flow-controlled
    XioSubmit::Queue requeue_q;
    XioMsg *xmsg;
    int isize = 0;

    while (q_iter != send_q.end()) {
      XioSubmit *xs = &(*q_iter);
      // skip retires and anything for other connections
      if (1 && xs->xcon != xcon) {
              q_iter++;
              continue;
      }
      xmsg = static_cast<XioMsg*>(xs);
      q_iter = send_q.erase(q_iter);
      requeue_q.push_back(*xmsg);
      xs->xcon->itry_count++;
      //break;
    }
    pthread_spin_lock(&xcon->sp);
    XioSubmit::Queue::const_iterator i1 = xcon->outgoing.requeue.end();
    xcon->outgoing.requeue.splice(i1, requeue_q);
    xcon->cstate.state_flow_controlled(XioConnection::CState::OP_FLAG_LOCKED);
    isize = xcon->outgoing.requeue.size();
    pthread_spin_unlock(&xcon->sp);

    if(isize > 1024)
    {
      ldout(msgr->cct,0) << "warning: close the remote conn["<< xcon << "]size["<< isize << "]" << dendl;
      //xcon->mark_down();
    }
  }
  
void *XioPortal::entry()
    {
      int size, code = 0;
      uint32_t xio_qdepth_high;
      XioSubmit::Queue send_q;
      XioSubmit::Queue::iterator q_iter;
      struct xio_msg *msg = NULL;
      XioConnection *xcon;
      XioSubmit *xs;
      XioMsg *xmsg;
      uint32_t isend_ctr = 0;
      XioMessenger *xmsgr = static_cast<XioMessenger*>(msgr);
      bool do_collection = msgr->cct->_conf->ms_status_collection;

      //usleep(1000*1000);
      xio_context_run_loop(ctx, 50);
      do {
	if(!this->shutdown_called.read())
        	submit_q.deq(send_q);

	/* shutdown() barrier */
	pthread_spin_lock(&sp);

      restart:
	size = send_q.size();

	if (_shutdown) {
	  // XXX XioMsg queues for flow-controlled connections may require
	  // cleanup
	  drained = true;
	}

	if (size > 0) {
	  q_iter = send_q.begin();
	  while (q_iter != send_q.end()) {
	    xs = &(*q_iter);
	    xcon = xs->xcon;

	    if(0 && xcon->itry_count > 64)
            {
              int q_size = xcon->outgoing.mqueue.size();
              xcon->cstate.state_up_ready(XioConnection::CState::OP_FLAG_NONE);
              ldout(msgr->cct,0) << "warning: flush the conn msg cache: conn[" << xcon << "]size["<< q_size << "]" <<dendl;
            }

	    switch (xs->type) {
	    case XioSubmit::OUTGOING_MSG: /* it was an outgoing 1-way */
	      xmsg = static_cast<XioMsg*>(xs);
	      if (unlikely(!xcon->conn || !xcon->is_connected()))
		code = ENOTCONN;
	      else {
		/* XXX guard Accelio send queue (should be safe to rely
		 * on Accelio's check on below, but this assures that
		 * all chained xio_msg are accounted) */
		xio_qdepth_high = xcon->xio_qdepth_high_mark();
                isend_ctr = __sync_fetch_and_add(&xcon->send_ctr, 0);
		__sync_fetch_and_add(&imsgcount, xmsg->hdr.msg_cnt);
		if (unlikely((isend_ctr + xmsg->hdr.msg_cnt) > xio_qdepth_high))
        {
		  //requeue_all_xcon(xcon, q_iter, send_q);
		  ldout(msgr->cct,0) << "warning: over flow to save: high["
                                     << xio_qdepth_high <<  "] send_ctr["
                                     << isend_ctr << "]conn["
                                     << xcon << "]cnt=["
                                     << xmsg->hdr.msg_cnt << "]" << dendl;
		  //goto restart;
		}
		msg = &xmsg->req_0.msg;
        code = -1;
        if(xcon->is_connected())
        {
	  if (do_collection) {
            msgr->cct->get_msstatus_collection()->add_msg_counts(xmsgr->get_mname(), 1);
            msgr->cct->get_msstatus_collection()->add_msg_bytes(xmsgr->get_mname(),
								xio_msg_bytes(msg));
          }
		  code = xio_send_msg(xcon->conn, msg);
        }
        else
        {
		  ldout(msgr->cct,0) << "warning: this connection is not valid.conn[" << xcon << "]" << dendl;
        }
				
		ldout(msgr->cct, 4) << " after xio_send_msg, code is: " << code << ", xmsg seq: " << xmsg->m->get_seq() << dendl;
		//usleep(10);
		/* header trace moved here to capture xio serial# */
		if (ldlog_p1(msgr->cct, ceph_subsys_xio, 11)) {
		  print_xio_msg_hdr(msgr->cct, "xio_send_msg", xmsg->hdr, msg);
		  print_ceph_msg(msgr->cct, "xio_send_msg", xmsg->m);
		}
		/* get the right Accelio's errno code */
		if (unlikely(code)) {
		  if ((code == -1) && (xio_errno() == -1)) {
		    /* In case XIO does not have any credits to send,
		     * it would still queue up the message(s) for transmission,
		     * but would return -1 and errno would also be set to -1.
		     * This needs to be treated as a success.
		     */
		     ldout(msgr->cct,0) << "warning: send msg error." << dendl;
		     code = 0;
		  }
		  else {
		    code = xio_errno();
		  }
		}
	      } /* !ENOTCONN */
	      if (unlikely(code)) {
		switch (code) {
		case XIO_E_TX_QUEUE_OVERFLOW:
		{
          int istat = 0;
          int iflag = xcon->is_connected();
		  istat = xcon->cstate.session_state.read();
		  requeue_all_xcon(xcon, q_iter, send_q);
		  ldout(msgr->cct,0) << " warning: accelio queue msg overflow.status["
                                     << istat << "]conn["
                                     << xcon <<"]msg["
                                     << msg << "]msgcount["
                                     << imsgcount << "]connected["
                                     << iflag << "]" << dendl;
		  goto restart;
		}
		  break;
		default:
		  q_iter = send_q.erase(q_iter);
		  xcon->msg_send_fail(xmsg, code);
		  continue;
		  break;
		};
	      } else {
		    xcon->send.set(msg->timestamp); // need atomic?
		    //xcon->send_ctr += xmsg->hdr.msg_cnt; // only inc if cb promised
            isend_ctr = __sync_fetch_and_add(&xcon->send_ctr, xmsg->hdr.msg_cnt);
	      }
	      break;
	    default:
	      /* INCOMING_MSG_RELEASE */
	      q_iter = send_q.erase(q_iter);
	      release_xio_rsp(static_cast<XioRsp*>(xs));
	      continue;
	    } /* switch (xs->type) */
	    q_iter = send_q.erase(q_iter);
	  } /* while */
	} /* size > 0 */

	pthread_spin_unlock(&sp);
	xio_context_run_loop(ctx, 50);

  } while ((!_shutdown) || (!drained));

  /* shutting down */
  if (server) {
	xio_unbind(server);
  }
  xio_context_destroy(ctx);
  return NULL;
}

