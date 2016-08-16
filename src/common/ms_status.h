// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_COMMON_MS_STATUS_H
#define CEPH_COMMON_MS_STATUS_H

#include "common/ceph_context.h"
#include <string>
using std::string;
#include <time.h>

/*
 * A MsStatus collection object, it is used for collection
 * ms informations.
 *
 * MsStatus can track several values:
 * 1) total connection numbers
 * 2) total msgs counts
 * 3) total msgs bytes
 * 4) avg msgs counts
 * 5) avg msgs bytes
 *
*/
class MsStatusCollection
{
public:
  MsStatusCollection(CephContext *cct) : m_cct(cct), conns(0),
    msgs_counts(0), msgs_bytes(0), last_msgs_counts(0),
    last_msgs_bytes(0)
  {
    last_time = time(NULL);
  }

  virtual ~MsStatusCollection() {}

  static MsStatusCollection *create_collection(CephContext *cct);

  virtual void dump_formatted(ceph::Formatter *f)
  {
    uint64_t tm = time(NULL);
    uint64_t use_time = tm - last_time;
    uint64_t now_mcounts = msgs_counts.read();
    uint64_t now_mbytes = msgs_bytes.read();
    uint64_t mcounts = now_mcounts - last_msgs_counts.read();
    uint64_t mbytes = now_mbytes - last_msgs_bytes.read();
    uint64_t avg_counts, avg_bytes = 0;

    if (use_time > 0) {
      avg_counts = mcounts/use_time;
      avg_bytes = mbytes/use_time;
    } else {
      avg_counts = last_avg_mcounts.read();
      avg_bytes = last_avg_mbytes.read();
    }

    f->open_object_section("total connection num");
    f->dump_unsigned("total connections", conns.read());
    f->close_section();
    f->open_object_section("total msgs num");
    f->dump_unsigned("total msgs counts", now_mcounts);
    f->dump_unsigned("total msgs bytes", now_mbytes);
    f->close_section();
    f->open_object_section("avg msgs num");
    f->dump_unsigned("avg msgs counts /s", avg_counts);
    f->dump_unsigned("avg msgs bytes /s", avg_bytes);
    f->close_section();

    last_time = tm;
    last_msgs_counts.set(now_mcounts);
    last_msgs_bytes.set(now_mbytes);
    last_avg_mcounts.set(avg_counts);
    last_avg_mbytes.set(avg_bytes);
  }

  virtual void dec_conns(string mname) { conns.dec(); }
  virtual void inc_conns(string mname) { conns.inc(); }
  virtual uint64_t get_conns() { return conns.read(); }

  virtual void add_msg_counts(string mname, uint64_t count)
  {
    uint64_t tmp = msgs_counts.read();
    tmp += count;
    msgs_counts.set(tmp);
  }

  virtual void add_msg_bytes(string mname, uint64_t count)
  {
    uint64_t tmp = msgs_bytes.read();
    tmp += count;
    msgs_bytes.set(tmp);
  }

private:
  CephContext *m_cct;
  atomic64_t conns;  /* total connection numbers */
  atomic64_t msgs_counts;  /* total msgs counts */
  atomic64_t msgs_bytes;  /* total msgs bytes */
  atomic64_t last_msgs_counts; /* last msgs counts */
  atomic64_t last_msgs_bytes; /* last msgs bytes */

  atomic64_t last_avg_mcounts; /* last avg msgs counts */
  atomic64_t last_avg_mbytes; /* last avg msgs bytes */
  uint64_t last_time; /* last collection time */
};

/* OSD status collection */
#define OSD_MSGR_NUM		6

#define OSD_MS_PUBLIC		0
#define OSD_MS_CLUSTER		1
#define OSD_MS_HBCLIENT		2
#define OSD_MS_HB_BACK_SERVER	3
#define OSD_MS_HB_FRONT_SERVER	4
#define OSD_MS_OBJECTER		5

/*
 * A OSD MsStatus collection object, it is used for collection
 * ms informations.
 *
 * MsStatus can track several values:
 * 1) messenger connection numbers
 * 2) messenger total msgs counts
 * 3) messenger total msgs bytes
 * 4) messenger avg msgs counts
 * 5) messenger avg msgs bytes
 *
*/
class OsdStatusCollection : public MsStatusCollection
{
public:
  OsdStatusCollection(CephContext *cct) : MsStatusCollection(cct)
  {
    int i = 0;
    for (i = 0; i < OSD_MSGR_NUM; i++) {
      set_mconn(i, 0);
      set_mcounts(i, 0);
      set_mbytes(i, 0);
      msgr_last_mct[i].set(0);
      msgr_last_byt[i].set(0);
      msgr_last_amct[i].set(0);
      msgr_last_abyt[i].set(0);
      msgr_last_time[i] = time(NULL);
    }
  }

  virtual ~OsdStatusCollection() {}

  void dump_formatted(ceph::Formatter *f);

  void dec_conns(string mname)
  {
    MsStatusCollection::dec_conns(mname);
    dec_mconn(get_pos_by_name(mname));
  }

  void inc_conns(string mname)
  {
    MsStatusCollection::inc_conns(mname);
    inc_mconn(get_pos_by_name(mname));
  }

  uint64_t get_mconns(int pos)
  {
    if (pos >= 0 && pos < OSD_MSGR_NUM)
      return mconn[pos].read();
    else
      return 0;
  }

  void add_msg_counts(string mname, uint64_t count)
  {
    MsStatusCollection::add_msg_counts(mname, count);
    add_msgr_mcounts(get_pos_by_name(mname), count);
  }

  void add_msg_bytes(string mname, uint64_t count)
  {
    MsStatusCollection::add_msg_bytes(mname, count);
    add_msgr_mbytes(get_pos_by_name(mname), count);
  }

  uint64_t get_mcounts(int pos)
  {
    if (pos >= 0 && pos < OSD_MSGR_NUM)
      return msgr_mcounts[pos].read();
    else
      return 0;
  }

  uint64_t get_mbytes(int pos)
  {
    if (pos >= 0 && pos < OSD_MSGR_NUM)
      return msgr_mbytes[pos].read();
    else
      return 0;
  }

private:
  atomic64_t mconn[OSD_MSGR_NUM];
  atomic64_t msgr_mcounts[OSD_MSGR_NUM];
  atomic64_t msgr_mbytes[OSD_MSGR_NUM];

  atomic64_t msgr_last_mct[OSD_MSGR_NUM]; /* last msgs counts */
  atomic64_t msgr_last_byt[OSD_MSGR_NUM]; /* last msgs bytes */

  atomic64_t msgr_last_amct[OSD_MSGR_NUM]; /* last avg msgs counts */
  atomic64_t msgr_last_abyt[OSD_MSGR_NUM]; /* last avg msgs bytes */
  uint64_t msgr_last_time[OSD_MSGR_NUM]; /* last collection time */

  int get_pos_by_name(string mname);

  void dump_client_formatted(ceph::Formatter *f);
  void dump_cluster_formatted(ceph::Formatter *f);
  void dump_hbclient_formatted(ceph::Formatter *f);
  void dump_hb_back_server_formatted(ceph::Formatter *f);
  void dump_hb_front_server_formatted(ceph::Formatter *f);
  void dump_ms_objecter_formatted(ceph::Formatter *f);

  void dec_mconn(int pos)
  {
    if (pos >=0 && pos < OSD_MSGR_NUM)
      mconn[pos].dec();
  }

  void inc_mconn(int pos)
  {
    if (pos >=0 && pos < OSD_MSGR_NUM)
      mconn[pos].inc();
  }

  void set_mconn(int pos, uint64_t val)
  {
    if (pos >=0 && pos < OSD_MSGR_NUM)
      mconn[pos].set(val);
  }

  void add_msgr_mcounts(int pos, uint64_t count)
  {
    uint64_t tmp = 0;
    if (pos >=0 && pos < OSD_MSGR_NUM) {
      tmp = msgr_mcounts[pos].read();
      tmp += count;
      msgr_mcounts[pos].set(tmp);
    }
  }

  void set_mcounts(int pos, uint64_t val)
  {
    if (pos >=0 && pos < OSD_MSGR_NUM)
      msgr_mcounts[pos].set(val);
  }

  void add_msgr_mbytes(int pos, uint64_t count)
  {
    uint64_t tmp = 0;
    if (pos >=0 && pos < OSD_MSGR_NUM) {
      tmp = msgr_mbytes[pos].read();
      tmp += count;
      msgr_mbytes[pos].set(tmp);
    }
  }

  void set_mbytes(int pos, uint64_t val)
  {
    if (pos >=0 && pos < OSD_MSGR_NUM)
      msgr_mbytes[pos].set(val);
  }
};

/* MON status collection */
#define MON_MSGR_NUM		1

#define MON_MS_MSGR		0

/*
 * A MON MsStatus collection object, it is used for collection
 * ms informations.
 *
 * MsStatus can track several values:
 * 1) messenger connection numbers
 * 2) messenger total msgs counts
 * 3) messenger total msgs bytes
 * 4) messenger avg msgs counts
 * 5) messenger avg msgs bytes
 *
*/
class MonStatusCollection : public MsStatusCollection
{
public:
  MonStatusCollection(CephContext *cct) : MsStatusCollection(cct)
  {
    int i = 0;
    for (i = 0; i < MON_MSGR_NUM; i++) {
      set_mconn(i, 0);
      set_mcounts(i, 0);
      set_mbytes(i, 0);
      msgr_last_mct[i].set(0);
      msgr_last_byt[i].set(0);
      msgr_last_amct[i].set(0);
      msgr_last_abyt[i].set(0);
      msgr_last_time[i] = time(NULL);
    }
  }

  virtual ~MonStatusCollection() {}

  void dump_formatted(ceph::Formatter *f);

  void dec_conns(string mname)
  {
    MsStatusCollection::dec_conns(mname);
    dec_mconn(get_pos_by_name(mname));
  }

  void inc_conns(string mname)
  {
    MsStatusCollection::inc_conns(mname);
    inc_mconn(get_pos_by_name(mname));
  }

  uint64_t get_mconns(int pos)
  {
    if (pos >= 0 && pos < MON_MSGR_NUM)
      return mconn[pos].read();
    else
      return 0;
  }

  void add_msg_counts(string mname, uint64_t count)
  {
    MsStatusCollection::add_msg_counts(mname, count);
    add_msgr_mcounts(get_pos_by_name(mname), count);
  }

  void add_msg_bytes(string mname, uint64_t count)
  {
    MsStatusCollection::add_msg_bytes(mname, count);
    add_msgr_mbytes(get_pos_by_name(mname), count);
  }

  uint64_t get_mcounts(int pos)
  {
    if (pos >= 0 && pos < MON_MSGR_NUM)
      return msgr_mcounts[pos].read();
    else
      return 0;
  }

  uint64_t get_mbytes(int pos)
  {
    if (pos >= 0 && pos < MON_MSGR_NUM)
      return msgr_mbytes[pos].read();
    else
      return 0;
  }

private:
  atomic64_t mconn[MON_MSGR_NUM];
  atomic64_t msgr_mcounts[MON_MSGR_NUM];
  atomic64_t msgr_mbytes[MON_MSGR_NUM];

  atomic64_t msgr_last_mct[MON_MSGR_NUM]; /* last msgs counts */
  atomic64_t msgr_last_byt[MON_MSGR_NUM]; /* last msgs bytes */

  atomic64_t msgr_last_amct[MON_MSGR_NUM]; /* last avg msgs counts */
  atomic64_t msgr_last_abyt[MON_MSGR_NUM]; /* last avg msgs bytes */
  uint64_t msgr_last_time[MON_MSGR_NUM]; /* last collection time */

  int get_pos_by_name(string mname);

  void dump_mon_formatted(ceph::Formatter *f);

  void dec_mconn(int pos)
  {
    if (pos >=0 && pos < MON_MSGR_NUM)
      mconn[pos].dec();
  }

  void inc_mconn(int pos)
  {
    if (pos >=0 && pos < MON_MSGR_NUM)
      mconn[pos].inc();
  }

  void set_mconn(int pos, uint64_t val)
  {
    if (pos >=0 && pos < MON_MSGR_NUM)
      mconn[pos].set(val);
  }

  void add_msgr_mcounts(int pos, uint64_t count)
  {
    uint64_t tmp = 0;
    if (pos >=0 && pos < MON_MSGR_NUM) {
      tmp = msgr_mcounts[pos].read();
      tmp += count;
      msgr_mcounts[pos].set(tmp);
    }
  }

  void set_mcounts(int pos, uint64_t val)
  {
    if (pos >=0 && pos < MON_MSGR_NUM)
      msgr_mcounts[pos].set(val);
  }

  void add_msgr_mbytes(int pos, uint64_t count)
  {
    uint64_t tmp = 0;
    if (pos >=0 && pos < MON_MSGR_NUM) {
      tmp = msgr_mbytes[pos].read();
      tmp += count;
      msgr_mbytes[pos].set(tmp);
    }
  }

  void set_mbytes(int pos, uint64_t val)
  {
    if (pos >=0 && pos < MON_MSGR_NUM)
      msgr_mbytes[pos].set(val);
  }
};

/* MDS status collection */
#define MDS_MSGR_NUM		1

#define MDS_MS_MSGR		0

/*
 * A MDS MsStatus collection object, it is used for collection
 * ms informations.
 *
 * MsStatus can track several values:
 * 1) messenger connection numbers
 * 2) messenger total msgs counts
 * 3) messenger total msgs bytes
 * 4) messenger avg msgs counts
 * 5) messenger avg msgs bytes
 *
*/
class MdsStatusCollection : public MsStatusCollection
{
public:
  MdsStatusCollection(CephContext *cct) : MsStatusCollection(cct)
  {
    int i = 0;
    for (i = 0; i < MDS_MSGR_NUM; i++) {
      set_mconn(i, 0);
      set_mcounts(i, 0);
      set_mbytes(i, 0);
      msgr_last_mct[i].set(0);
      msgr_last_byt[i].set(0);
      msgr_last_amct[i].set(0);
      msgr_last_abyt[i].set(0);
      msgr_last_time[i] = time(NULL);
    }
  }

  virtual ~MdsStatusCollection() {}

  void dump_formatted(ceph::Formatter *f);

  void dec_conns(string mname)
  {
    MsStatusCollection::dec_conns(mname);
    dec_mconn(get_pos_by_name(mname));
  }

  void inc_conns(string mname)
  {
    MsStatusCollection::inc_conns(mname);
    inc_mconn(get_pos_by_name(mname));
  }

  uint64_t get_mconns(int pos)
  {
    if (pos >= 0 && pos < MDS_MSGR_NUM)
      return mconn[pos].read();
    else
      return 0;
  }

  void add_msg_counts(string mname, uint64_t count)
  {
    MsStatusCollection::add_msg_counts(mname, count);
    add_msgr_mcounts(get_pos_by_name(mname), count);
  }

  void add_msg_bytes(string mname, uint64_t count)
  {
    MsStatusCollection::add_msg_bytes(mname, count);
    add_msgr_mbytes(get_pos_by_name(mname), count);
  }

  uint64_t get_mcounts(int pos)
  {
    if (pos >= 0 && pos < MDS_MSGR_NUM)
      return msgr_mcounts[pos].read();
    else
      return 0;
  }

  uint64_t get_mbytes(int pos)
  {
    if (pos >= 0 && pos < MDS_MSGR_NUM)
      return msgr_mbytes[pos].read();
    else
      return 0;
  }

private:
  atomic64_t mconn[MDS_MSGR_NUM];
  atomic64_t msgr_mcounts[MDS_MSGR_NUM];
  atomic64_t msgr_mbytes[MDS_MSGR_NUM];

  atomic64_t msgr_last_mct[MDS_MSGR_NUM]; /* last msgs counts */
  atomic64_t msgr_last_byt[MDS_MSGR_NUM]; /* last msgs bytes */

  atomic64_t msgr_last_amct[MDS_MSGR_NUM]; /* last avg msgs counts */
  atomic64_t msgr_last_abyt[MDS_MSGR_NUM]; /* last avg msgs bytes */
  uint64_t msgr_last_time[MDS_MSGR_NUM]; /* last collection time */

  int get_pos_by_name(string mname);

  void dump_mds_formatted(ceph::Formatter *f);

  void dec_mconn(int pos)
  {
    if (pos >=0 && pos < MDS_MSGR_NUM)
      mconn[pos].dec();
  }

  void inc_mconn(int pos)
  {
    if (pos >=0 && pos < MDS_MSGR_NUM)
      mconn[pos].inc();
  }

  void set_mconn(int pos, uint64_t val)
  {
    if (pos >=0 && pos < MDS_MSGR_NUM)
      mconn[pos].set(val);
  }

  void add_msgr_mcounts(int pos, uint64_t count)
  {
    uint64_t tmp = 0;
    if (pos >=0 && pos < MDS_MSGR_NUM) {
      tmp = msgr_mcounts[pos].read();
      tmp += count;
      msgr_mcounts[pos].set(tmp);
    }
  }

  void set_mcounts(int pos, uint64_t val)
  {
    if (pos >=0 && pos < MDS_MSGR_NUM)
      msgr_mcounts[pos].set(val);
  }

  void add_msgr_mbytes(int pos, uint64_t count)
  {
    uint64_t tmp = 0;
    if (pos >=0 && pos < MDS_MSGR_NUM) {
      tmp = msgr_mbytes[pos].read();
      tmp += count;
      msgr_mbytes[pos].set(tmp);
    }
  }

  void set_mbytes(int pos, uint64_t val)
  {
    if (pos >=0 && pos < MDS_MSGR_NUM)
      msgr_mbytes[pos].set(val);
  }
};

/* Client status collection */
#define CLIENT_MSGR_NUM		1

#define CLIENT_MS_MSGR		0

/*
 * A Client MsStatus collection object, it is used for collection
 * ms informations.
 *
 * MsStatus can track several values:
 * 1) messenger connection numbers
 * 2) messenger total msgs counts
 * 3) messenger total msgs bytes
 * 4) messenger avg msgs counts
 * 5) messenger avg msgs bytes
 *
*/
class ClientStatusCollection : public MsStatusCollection
{
public:
  ClientStatusCollection(CephContext *cct) : MsStatusCollection(cct)
  {
    int i = 0;
    for (i = 0; i < CLIENT_MSGR_NUM; i++) {
      set_mconn(i, 0);
      set_mcounts(i, 0);
      set_mbytes(i, 0);
      msgr_last_mct[i].set(0);
      msgr_last_byt[i].set(0);
      msgr_last_amct[i].set(0);
      msgr_last_abyt[i].set(0);
      msgr_last_time[i] = time(NULL);
    }
  }

  virtual ~ClientStatusCollection() {}

  void dump_formatted(ceph::Formatter *f);

  void dec_conns(string mname)
  {
    MsStatusCollection::dec_conns(mname);
    dec_mconn(get_pos_by_name(mname));
  }

  void inc_conns(string mname)
  {
    MsStatusCollection::inc_conns(mname);
    inc_mconn(get_pos_by_name(mname));
  }

  uint64_t get_mconns(int pos)
  {
    if (pos >= 0 && pos < CLIENT_MSGR_NUM)
      return mconn[pos].read();
    else
      return 0;
  }

  void add_msg_counts(string mname, uint64_t count)
  {
    MsStatusCollection::add_msg_counts(mname, count);
    add_msgr_mcounts(get_pos_by_name(mname), count);
  }

  void add_msg_bytes(string mname, uint64_t count)
  {
    MsStatusCollection::add_msg_bytes(mname, count);
    add_msgr_mbytes(get_pos_by_name(mname), count);
  }

  uint64_t get_mcounts(int pos)
  {
    if (pos >= 0 && pos < CLIENT_MSGR_NUM)
      return msgr_mcounts[pos].read();
    else
      return 0;
  }

  uint64_t get_mbytes(int pos)
  {
    if (pos >= 0 && pos < CLIENT_MSGR_NUM)
      return msgr_mbytes[pos].read();
    else
      return 0;
  }

private:
  atomic64_t mconn[CLIENT_MSGR_NUM];
  atomic64_t msgr_mcounts[CLIENT_MSGR_NUM];
  atomic64_t msgr_mbytes[CLIENT_MSGR_NUM];

  atomic64_t msgr_last_mct[CLIENT_MSGR_NUM]; /* last msgs counts */
  atomic64_t msgr_last_byt[CLIENT_MSGR_NUM]; /* last msgs bytes */

  atomic64_t msgr_last_amct[CLIENT_MSGR_NUM]; /* last avg msgs counts */
  atomic64_t msgr_last_abyt[CLIENT_MSGR_NUM]; /* last avg msgs bytes */
  uint64_t msgr_last_time[CLIENT_MSGR_NUM]; /* last collection time */

  int get_pos_by_name(string mname);

  void dump_client_formatted(ceph::Formatter *f);

  void dec_mconn(int pos)
  {
    if (pos >=0 && pos < CLIENT_MSGR_NUM)
      mconn[pos].dec();
  }

  void inc_mconn(int pos)
  {
    if (pos >=0 && pos < CLIENT_MSGR_NUM)
      mconn[pos].inc();
  }

  void set_mconn(int pos, uint64_t val)
  {
    if (pos >=0 && pos < CLIENT_MSGR_NUM)
      mconn[pos].set(val);
  }

  void add_msgr_mcounts(int pos, uint64_t count)
  {
    uint64_t tmp = 0;
    if (pos >=0 && pos < CLIENT_MSGR_NUM) {
      tmp = msgr_mcounts[pos].read();
      tmp += count;
      msgr_mcounts[pos].set(tmp);
    }
  }

  void set_mcounts(int pos, uint64_t val)
  {
    if (pos >=0 && pos < CLIENT_MSGR_NUM)
      msgr_mcounts[pos].set(val);
  }

  void add_msgr_mbytes(int pos, uint64_t count)
  {
    uint64_t tmp = 0;
    if (pos >=0 && pos < CLIENT_MSGR_NUM) {
      tmp = msgr_mbytes[pos].read();
      tmp += count;
      msgr_mbytes[pos].set(tmp);
    }
  }

  void set_mbytes(int pos, uint64_t val)
  {
    if (pos >=0 && pos < CLIENT_MSGR_NUM)
      msgr_mbytes[pos].set(val);
  }
};
#endif /* end ifndef */
