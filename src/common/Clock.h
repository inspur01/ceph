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

#ifndef CEPH_CLOCK_H
#define CEPH_CLOCK_H

#include "include/utime.h"

#include <time.h>
#include <fstream>
using namespace std;
class CephContext;

extern utime_t ceph_clock_now(CephContext *cct);
extern time_t ceph_clock_gettime(CephContext *cct);
extern uint64_t get_jiffies();
extern long get_cycle();
extern uint64_t cpu_hz;

struct ceph_log_info {
  long long beginstamp;
  int  logtime[500];
  int  logname[500];
  int  logindex;
} __attribute__ ((packed));
extern map<pthread_t,ceph_log_info*> g_plg;

extern void print_trace (ofstream &ofs,void *p,char *pc = NULL);

#define INITCEPHLOG()     struct ceph_log_info *lpcephlog = NULL; \
        if(!g_plg.empty())    { \
          std::map<pthread_t,ceph_log_info*>::iterator ipos = g_plg.find(pthread_self()); \
          if(ipos != g_plg.end())   \
          lpcephlog = ipos->second; \
        }

/*  if(g_plg.empty()) break;      \
  ceph_log_info *lpcephlog = g_plg[pthread_self()]; \
 */

#define WCEPHLOG(index)   do { \
  if(lpcephlog == NULL || lpcephlog->logindex >= 99)break; \
  lpcephlog->logname[lpcephlog->logindex] = index; \
  lpcephlog->logtime[lpcephlog->logindex++] = (get_jiffies() - lpcephlog->beginstamp)/cpu_hz;  \
}while(0);

#endif

