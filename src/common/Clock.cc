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


#include "common/Clock.h"
#include "common/ceph_context.h"
#include "common/config.h"
#include "include/utime.h"

#include <time.h>

utime_t ceph_clock_now(CephContext *cct)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  utime_t n(&tv);
  if (cct)
    n += cct->_conf->clock_offset;
  return n;
}

time_t ceph_clock_gettime(CephContext *cct)
{
  time_t ret = time(NULL);
  if (cct)
    ret += ((time_t)cct->_conf->clock_offset);
  return ret;
}
uint64_t get_jiffies()
{
  unsigned int aux;
  uint64_t low, high;
  asm volatile(".byte 0x0f,0x01,0xf9" : "=a"(low), "=d"(high), "=c"(aux));
  return low | (high << 32);
}

long get_cycle(){
  uint64_t start = 0 ;
  uint64_t end = 0 ;   
  start  = get_jiffies() ;  
  sleep(1);
  end = get_jiffies() ;
  return (end - start)/1000000ULL ;
}
uint64_t cpu_hz;
map<pthread_t,ceph_log_info*> g_plg; 
void print_trace (ofstream &ofs,void *p,char *pc)
{
  void *array[20];
  size_t size;
  char **strings;
  size_t i;
  size = backtrace(array,20);
  strings = backtrace_symbols (array, size);
  ofs<<"SIG: "<<(pc==NULL?"":pc)<<" faddr: "<<p<<" Obtained " <<size<<" stack frames."<<std::endl;
  for (i = 0; i < size; i++)
    ofs<<strings[i]<<std::endl;
  free (strings);
}


