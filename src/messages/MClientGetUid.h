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

#ifndef CEPH_MCLIENTGETUID_H
#define CEPH_MCLIENTGETUID_H

#include "msg/Message.h"

#include "include/types.h"

class MClientGetUid : public Message {
public:
  int bauth;
  string username;
  string passwd;
  MClientGetUid() : Message(CEPH_MSG_GET_CLIENT_UID) { }
private:
  ~MClientGetUid() {}

public:
  const char *get_type_name() const { return "mon_getuid"; }

  void print(ostream& out) const {
    out << "user name (" << username << ")";
  }
  
  void encode_payload(uint64_t features) { 
    ::encode(bauth, payload);
    ::encode(username, payload);
    ::encode(passwd, payload);
  }
  void decode_payload() { 
    bufferlist::iterator p = payload.begin();
    ::decode(bauth, p);
    ::decode(username, p);
    ::decode(passwd, p);
  }
};

#endif

