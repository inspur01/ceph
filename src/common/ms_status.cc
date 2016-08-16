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

#include "common/ms_status.h"
#include "include/msgr.h"

MsStatusCollection *MsStatusCollection::create_collection(CephContext *cct)
{
  MsStatusCollection *ms = NULL;

  switch (cct->get_module_type()) {
    case CEPH_ENTITY_TYPE_OSD:
      ms = new OsdStatusCollection(cct);
      break;
    case CEPH_ENTITY_TYPE_MON:
      ms = new MonStatusCollection(cct);
      break;
    case CEPH_ENTITY_TYPE_MDS:
      ms = new MdsStatusCollection(cct);
      break;
    case CEPH_ENTITY_TYPE_CLIENT:
      ms = new ClientStatusCollection(cct);
      break;
    default:
      ms = new MsStatusCollection(cct);
      break;
  }
  return ms;
}

/* OSD status collection */
void OsdStatusCollection::dump_client_formatted(ceph::Formatter *f)
{
  uint64_t tm = time(NULL);
  uint64_t use_time = tm - msgr_last_time[OSD_MS_PUBLIC];
  uint64_t now_mcounts = get_mcounts(OSD_MS_PUBLIC);
  uint64_t now_mbytes = get_mbytes(OSD_MS_PUBLIC);
  uint64_t mcounts = now_mcounts - msgr_last_mct[OSD_MS_PUBLIC].read();
  uint64_t mbytes = now_mbytes - msgr_last_byt[OSD_MS_PUBLIC].read();
  uint64_t avg_counts, avg_bytes = 0;

  if (use_time > 0) {
    avg_counts = mcounts/use_time;
    avg_bytes = mbytes/use_time;
  } else {
    avg_counts = msgr_last_amct[OSD_MS_PUBLIC].read();
    avg_bytes = msgr_last_abyt[OSD_MS_PUBLIC].read();
  }

  f->open_object_section("client msgr info");
  f->dump_unsigned("client conns", get_mconns(OSD_MS_PUBLIC));
  f->dump_unsigned("client msg counts", now_mcounts);
  f->dump_unsigned("client msg bytes", now_mbytes);
  f->dump_unsigned("client avg msg counts /s", avg_counts);
  f->dump_unsigned("client avg msg bytes /s", avg_bytes);
  f->close_section();

  msgr_last_time[OSD_MS_PUBLIC] = tm;
  msgr_last_mct[OSD_MS_PUBLIC].set(now_mcounts);
  msgr_last_byt[OSD_MS_PUBLIC].set(now_mbytes);
  msgr_last_amct[OSD_MS_PUBLIC].set(avg_counts);
  msgr_last_abyt[OSD_MS_PUBLIC].set(avg_bytes);
}

void OsdStatusCollection::dump_cluster_formatted(ceph::Formatter *f)
{
  uint64_t tm = time(NULL);
  uint64_t use_time = tm - msgr_last_time[OSD_MS_CLUSTER];
  uint64_t now_mcounts = get_mcounts(OSD_MS_CLUSTER);
  uint64_t now_mbytes = get_mbytes(OSD_MS_CLUSTER);
  uint64_t mcounts = now_mcounts - msgr_last_mct[OSD_MS_CLUSTER].read();
  uint64_t mbytes = now_mbytes - msgr_last_byt[OSD_MS_CLUSTER].read();
  uint64_t avg_counts, avg_bytes = 0;

  if (use_time > 0) {
    avg_counts = mcounts/use_time;
    avg_bytes = mbytes/use_time;
  } else {
    avg_counts = msgr_last_amct[OSD_MS_CLUSTER].read();
    avg_bytes = msgr_last_abyt[OSD_MS_CLUSTER].read();
  }

  f->open_object_section("cluster msgr info");
  f->dump_unsigned("cluster conns", get_mconns(OSD_MS_CLUSTER));
  f->dump_unsigned("cluster msg counts", now_mcounts);
  f->dump_unsigned("cluster msg bytes", now_mbytes);
  f->dump_unsigned("cluster avg msg counts /s", avg_counts);
  f->dump_unsigned("cluster avg msg bytes /s", avg_bytes);
  f->close_section();

  msgr_last_time[OSD_MS_CLUSTER] = tm;
  msgr_last_mct[OSD_MS_CLUSTER].set(now_mcounts);
  msgr_last_byt[OSD_MS_CLUSTER].set(now_mbytes);
  msgr_last_amct[OSD_MS_CLUSTER].set(avg_counts);
  msgr_last_abyt[OSD_MS_CLUSTER].set(avg_bytes);
}

void OsdStatusCollection::dump_hbclient_formatted(ceph::Formatter *f)
{
  uint64_t tm = time(NULL);
  uint64_t use_time = tm - msgr_last_time[OSD_MS_HBCLIENT];
  uint64_t now_mcounts = get_mcounts(OSD_MS_HBCLIENT);
  uint64_t now_mbytes = get_mbytes(OSD_MS_HBCLIENT);
  uint64_t mcounts = now_mcounts - msgr_last_mct[OSD_MS_HBCLIENT].read();
  uint64_t mbytes = now_mbytes - msgr_last_byt[OSD_MS_HBCLIENT].read();
  uint64_t avg_counts, avg_bytes = 0;

  if (use_time > 0) {
    avg_counts = mcounts/use_time;
    avg_bytes = mbytes/use_time;
  } else {
    avg_counts = msgr_last_amct[OSD_MS_HBCLIENT].read();
    avg_bytes = msgr_last_abyt[OSD_MS_HBCLIENT].read();
  }

  f->open_object_section("hbclient msgr info");
  f->dump_unsigned("hbclient conns", get_mconns(OSD_MS_HBCLIENT));
  f->dump_unsigned("hbclient msg counts", now_mcounts);
  f->dump_unsigned("hbclient msg bytes", now_mbytes);
  f->dump_unsigned("hbclient avg msg counts /s", avg_counts);
  f->dump_unsigned("hbclient avg msg bytes /s", avg_bytes);
  f->close_section();

  msgr_last_time[OSD_MS_HBCLIENT] = tm;
  msgr_last_mct[OSD_MS_HBCLIENT].set(now_mcounts);
  msgr_last_byt[OSD_MS_HBCLIENT].set(now_mbytes);
  msgr_last_amct[OSD_MS_HBCLIENT].set(avg_counts);
  msgr_last_abyt[OSD_MS_HBCLIENT].set(avg_bytes);
}

void OsdStatusCollection::dump_hb_back_server_formatted(ceph::Formatter *f)
{
  uint64_t tm = time(NULL);
  uint64_t use_time = tm - msgr_last_time[OSD_MS_HB_BACK_SERVER];
  uint64_t now_mcounts = get_mcounts(OSD_MS_HB_BACK_SERVER);
  uint64_t now_mbytes = get_mbytes(OSD_MS_HB_BACK_SERVER);
  uint64_t mcounts = now_mcounts - msgr_last_mct[OSD_MS_HB_BACK_SERVER].read();
  uint64_t mbytes = now_mbytes - msgr_last_byt[OSD_MS_HB_BACK_SERVER].read();
  uint64_t avg_counts, avg_bytes = 0;

  if (use_time > 0) {
    avg_counts = mcounts/use_time;
    avg_bytes = mbytes/use_time;
  } else {
    avg_counts = msgr_last_amct[OSD_MS_HB_BACK_SERVER].read();
    avg_bytes = msgr_last_abyt[OSD_MS_HB_BACK_SERVER].read();
  }

  f->open_object_section("hb_back_server msgr info");
  f->dump_unsigned("hb_back_server conns", get_mconns(OSD_MS_HB_BACK_SERVER));
  f->dump_unsigned("hb_back_server msg counts", now_mcounts);
  f->dump_unsigned("hb_back_server msg bytes", now_mbytes);
  f->dump_unsigned("hb_back_server avg msg counts /s", avg_counts);
  f->dump_unsigned("hb_back_server avg msg bytes /s", avg_bytes);
  f->close_section();

  msgr_last_time[OSD_MS_HB_BACK_SERVER] = tm;
  msgr_last_mct[OSD_MS_HB_BACK_SERVER].set(now_mcounts);
  msgr_last_byt[OSD_MS_HB_BACK_SERVER].set(now_mbytes);
  msgr_last_amct[OSD_MS_HB_BACK_SERVER].set(avg_counts);
  msgr_last_abyt[OSD_MS_HB_BACK_SERVER].set(avg_bytes);
}

void OsdStatusCollection::dump_hb_front_server_formatted(ceph::Formatter *f)
{
  uint64_t tm = time(NULL);
  uint64_t use_time = tm - msgr_last_time[OSD_MS_HB_FRONT_SERVER];
  uint64_t now_mcounts = get_mcounts(OSD_MS_HB_FRONT_SERVER);
  uint64_t now_mbytes = get_mbytes(OSD_MS_HB_FRONT_SERVER);
  uint64_t mcounts = now_mcounts - msgr_last_mct[OSD_MS_HB_FRONT_SERVER].read();
  uint64_t mbytes = now_mbytes - msgr_last_byt[OSD_MS_HB_FRONT_SERVER].read();
  uint64_t avg_counts, avg_bytes = 0;

  if (use_time > 0) {
    avg_counts = mcounts/use_time;
    avg_bytes = mbytes/use_time;
  } else {
    avg_counts = msgr_last_amct[OSD_MS_HB_FRONT_SERVER].read();
    avg_bytes = msgr_last_abyt[OSD_MS_HB_FRONT_SERVER].read();
  }

  f->open_object_section("hb_front_server msgr info");
  f->dump_unsigned("hb_front_server conns", get_mconns(OSD_MS_HB_FRONT_SERVER));
  f->dump_unsigned("hb_front_server msg counts", now_mcounts);
  f->dump_unsigned("hb_front_server msg bytes", now_mbytes);
  f->dump_unsigned("hb_front_server avg msg counts /s", avg_counts);
  f->dump_unsigned("hb_front_server avg msg bytes /s", avg_bytes);
  f->close_section();

  msgr_last_time[OSD_MS_HB_FRONT_SERVER] = tm;
  msgr_last_mct[OSD_MS_HB_FRONT_SERVER].set(now_mcounts);
  msgr_last_byt[OSD_MS_HB_FRONT_SERVER].set(now_mbytes);
  msgr_last_amct[OSD_MS_HB_FRONT_SERVER].set(avg_counts);
  msgr_last_abyt[OSD_MS_HB_FRONT_SERVER].set(avg_bytes);
}

void OsdStatusCollection::dump_ms_objecter_formatted(ceph::Formatter *f)
{
  uint64_t tm = time(NULL);
  uint64_t use_time = tm - msgr_last_time[OSD_MS_OBJECTER];
  uint64_t now_mcounts = get_mcounts(OSD_MS_OBJECTER);
  uint64_t now_mbytes = get_mbytes(OSD_MS_OBJECTER);
  uint64_t mcounts = now_mcounts - msgr_last_mct[OSD_MS_OBJECTER].read();
  uint64_t mbytes = now_mbytes - msgr_last_byt[OSD_MS_OBJECTER].read();
  uint64_t avg_counts, avg_bytes = 0;

  if (use_time > 0) {
    avg_counts = mcounts/use_time;
    avg_bytes = mbytes/use_time;
  } else {
    avg_counts = msgr_last_amct[OSD_MS_OBJECTER].read();
    avg_bytes = msgr_last_abyt[OSD_MS_OBJECTER].read();
  }

  f->open_object_section("ms_objecter msgr info");
  f->dump_unsigned("ms_objecter conns", get_mconns(OSD_MS_OBJECTER));
  f->dump_unsigned("ms_objecter msg counts", now_mcounts);
  f->dump_unsigned("ms_objecter msg bytes", now_mbytes);
  f->dump_unsigned("ms_objecter avg msg counts /s", avg_counts);
  f->dump_unsigned("ms_objecter avg msg bytes /s", avg_bytes);
  f->close_section();

  msgr_last_time[OSD_MS_OBJECTER] = tm;
  msgr_last_mct[OSD_MS_OBJECTER].set(now_mcounts);
  msgr_last_byt[OSD_MS_OBJECTER].set(now_mbytes);
  msgr_last_amct[OSD_MS_OBJECTER].set(avg_counts);
  msgr_last_abyt[OSD_MS_OBJECTER].set(avg_bytes);
}

void OsdStatusCollection::dump_formatted(ceph::Formatter *f)
{
  f->open_array_section("type: osd");
  f->dump_stream("type") << "type osd";
  MsStatusCollection::dump_formatted(f);

  dump_client_formatted(f);
  dump_cluster_formatted(f);
  dump_hbclient_formatted(f);
  dump_hb_back_server_formatted(f);
  dump_hb_front_server_formatted(f);
  dump_ms_objecter_formatted(f);

  f->close_section();
}

int OsdStatusCollection::get_pos_by_name(string mname)
{
  int pos = -1;

  if (mname.compare("client") == 0)
    pos = OSD_MS_PUBLIC;
  else if (mname.compare("cluster") == 0)
    pos = OSD_MS_CLUSTER;
  else if (mname.compare("hbclient") == 0)
    pos = OSD_MS_HBCLIENT;
  else if (mname.compare("hb_back_server") == 0)
    pos = OSD_MS_HB_BACK_SERVER;
  else if (mname.compare("hb_front_server") == 0)
    pos = OSD_MS_HB_FRONT_SERVER;
  else if (mname.compare("ms_objecter") == 0)
    pos = OSD_MS_OBJECTER;

  return pos;
}

/* MON status collection */
void MonStatusCollection::dump_mon_formatted(ceph::Formatter *f)
{
  uint64_t tm = time(NULL);
  uint64_t use_time = tm - msgr_last_time[MON_MS_MSGR];
  uint64_t now_mcounts = get_mcounts(MON_MS_MSGR);
  uint64_t now_mbytes = get_mbytes(MON_MS_MSGR);
  uint64_t mcounts = now_mcounts - msgr_last_mct[MON_MS_MSGR].read();
  uint64_t mbytes = now_mbytes - msgr_last_byt[MON_MS_MSGR].read();
  uint64_t avg_counts, avg_bytes = 0;

  if (use_time > 0) {
    avg_counts = mcounts/use_time;
    avg_bytes = mbytes/use_time;
  } else {
    avg_counts = msgr_last_amct[MON_MS_MSGR].read();
    avg_bytes = msgr_last_abyt[MON_MS_MSGR].read();
  }

  f->open_object_section("msgr connection num");
  f->dump_unsigned("mon msgr conns", get_mconns(MON_MS_MSGR));
  f->dump_unsigned("mon msgr msg counts", now_mcounts);
  f->dump_unsigned("mon msgr msg bytes", now_mbytes);
  f->dump_unsigned("mon msgr avg msg counts /s", avg_counts);
  f->dump_unsigned("mon msgr avg msg bytes /s", avg_bytes);
  f->close_section();

  msgr_last_time[MON_MS_MSGR] = tm;
  msgr_last_mct[MON_MS_MSGR].set(now_mcounts);
  msgr_last_byt[MON_MS_MSGR].set(now_mbytes);
  msgr_last_amct[MON_MS_MSGR].set(avg_counts);
  msgr_last_abyt[MON_MS_MSGR].set(avg_bytes);
}

void MonStatusCollection::dump_formatted(ceph::Formatter *f)
{
  f->open_array_section("type: mon");
  f->dump_stream("type") << "type mon";
  MsStatusCollection::dump_formatted(f);

  dump_mon_formatted(f);

  f->close_section();
}

int MonStatusCollection::get_pos_by_name(string mname)
{
  int pos = -1;

  if (mname.compare("mon") == 0)
    pos = MON_MS_MSGR;

  return pos;
}

/* MDS status collection */
void MdsStatusCollection::dump_mds_formatted(ceph::Formatter *f)
{
  uint64_t tm = time(NULL);
  uint64_t use_time = tm - msgr_last_time[MDS_MS_MSGR];
  uint64_t now_mcounts = get_mcounts(MDS_MS_MSGR);
  uint64_t now_mbytes = get_mbytes(MDS_MS_MSGR);
  uint64_t mcounts = now_mcounts - msgr_last_mct[MDS_MS_MSGR].read();
  uint64_t mbytes = now_mbytes - msgr_last_byt[MDS_MS_MSGR].read();
  uint64_t avg_counts, avg_bytes = 0;

  if (use_time > 0) {
    avg_counts = mcounts/use_time;
    avg_bytes = mbytes/use_time;
  } else {
    avg_counts = msgr_last_amct[MDS_MS_MSGR].read();
    avg_bytes = msgr_last_abyt[MDS_MS_MSGR].read();
  }

  f->open_object_section("msgr connection num");
  f->dump_unsigned("mds msgr conns", get_mconns(MDS_MS_MSGR));
  f->dump_unsigned("mds msgr msg counts", now_mcounts);
  f->dump_unsigned("mds msgr msg bytes", now_mbytes);
  f->dump_unsigned("mds msgr avg msg counts /s", avg_counts);
  f->dump_unsigned("mds msgr avg msg bytes /s", avg_bytes);
  f->close_section();

  msgr_last_time[MDS_MS_MSGR] = tm;
  msgr_last_mct[MDS_MS_MSGR].set(now_mcounts);
  msgr_last_byt[MDS_MS_MSGR].set(now_mbytes);
  msgr_last_amct[MDS_MS_MSGR].set(avg_counts);
  msgr_last_abyt[MDS_MS_MSGR].set(avg_bytes);
}

void MdsStatusCollection::dump_formatted(ceph::Formatter *f)
{
  f->open_array_section("type: mds");
  f->dump_stream("type") << "type mds";
  MsStatusCollection::dump_formatted(f);

  dump_mds_formatted(f);

  f->close_section();
}

int MdsStatusCollection::get_pos_by_name(string mname)
{
  int pos = -1;

  if (mname.compare("mds") == 0)
    pos = MDS_MS_MSGR;

  return pos;
}

/* Client status collection */
void ClientStatusCollection::dump_client_formatted(ceph::Formatter *f)
{
  uint64_t tm = time(NULL);
  uint64_t use_time = tm - msgr_last_time[CLIENT_MS_MSGR];
  uint64_t now_mcounts = get_mcounts(CLIENT_MS_MSGR);
  uint64_t now_mbytes = get_mbytes(CLIENT_MS_MSGR);
  uint64_t mcounts = now_mcounts - msgr_last_mct[CLIENT_MS_MSGR].read();
  uint64_t mbytes = now_mbytes - msgr_last_byt[CLIENT_MS_MSGR].read();
  uint64_t avg_counts, avg_bytes = 0;

  if (use_time > 0) {
    avg_counts = mcounts/use_time;
    avg_bytes = mbytes/use_time;
  } else {
    avg_counts = msgr_last_amct[CLIENT_MS_MSGR].read();
    avg_bytes = msgr_last_abyt[CLIENT_MS_MSGR].read();
  }

  f->open_object_section("msgr connection num");
  f->dump_unsigned("client msgr conns", get_mconns(CLIENT_MS_MSGR));
  f->dump_unsigned("client msgr msg counts", now_mcounts);
  f->dump_unsigned("client msgr msg bytes", now_mbytes);
  f->dump_unsigned("client msgr avg msg counts /s", avg_counts);
  f->dump_unsigned("client msgr avg msg bytes /s", avg_bytes);
  f->close_section();

  msgr_last_time[CLIENT_MS_MSGR] = tm;
  msgr_last_mct[CLIENT_MS_MSGR].set(now_mcounts);
  msgr_last_byt[CLIENT_MS_MSGR].set(now_mbytes);
  msgr_last_amct[CLIENT_MS_MSGR].set(avg_counts);
  msgr_last_abyt[CLIENT_MS_MSGR].set(avg_bytes);
}

void ClientStatusCollection::dump_formatted(ceph::Formatter *f)
{
  f->open_array_section("type: client");
  f->dump_stream("type") << "type client";
  MsStatusCollection::dump_formatted(f);

  dump_client_formatted(f);

  f->close_section();
}

int ClientStatusCollection::get_pos_by_name(string mname)
{
  int pos = -1;

  if (mname.compare("client") == 0)
    pos = CLIENT_MS_MSGR;

  return pos;
}
