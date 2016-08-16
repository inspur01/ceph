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
 * zzg
 */

#ifndef CEPH_MCLIENTUIDREPLY_H
#define CEPH_MCLIENTUIDREPLY_H

#include "include/ceph_features.h"
#include "msg/Message.h"

class MClientUidReply : public Message {
public:
  int authenticated;
  int uid;
  int gid;

  MClientUidReply() : Message(CEPH_MSG_CLIENT_UID_REPLY) { }
  MClientUidReply( int a, int u, int g) 
  	: Message(CEPH_MSG_CLIENT_UID_REPLY), 
  	authenticated(a), uid(u), gid(g) { }
private:
  ~MClientUidReply() {}

public:
  const char *get_type_name() const { return "mon_winuid"; }
  void print(ostream& o) const {
    o << "uid: " << uid << "gid: " << gid << "~";
  }

  void encode_payload(uint64_t features) { 
    ::encode(authenticated, payload);
    ::encode(uid, payload);
    ::encode(gid, payload);
  }
  void decode_payload() { 
    bufferlist::iterator p = payload.begin();
    ::decode(authenticated, p);
    ::decode(uid, p);
    ::decode(gid, p);
  }
};
#endif

