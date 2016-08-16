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

/* 
 * This is the top level monitor. It runs on each machine in the Monitor   
 * Cluster. The election of a leader for the paxos algorithm only happens 
 * once per machine via the elector. There is a separate paxos instance (state) 
 * kept for each of the system components: Object Store Device (OSD) Monitor, 
 * Placement Group (PG) Monitor, Metadata Server (MDS) Monitor, and Client Monitor.
 */

#ifndef CEPH_GRANTMONITOR_H
#define CEPH_GRANTMONITOR_H

#include <map>
#include <set>
using namespace std;

#include <boost/lexical_cast.hpp>

#include "include/ceph_features.h"
#include "include/types.h"
#include "msg/Messenger.h"
#include "mon/Monitor.h"
#include "mon/PaxosService.h"
#include "mon/MonitorDBStore.h"
#include <cstdlib>    //add by zhanghao
#include <uuid/uuid.h>  //add by zhanghao
#include <stdio.h>  //add by zhanghao 

class MMonCommand;

class Monitor ;
struct KeyServerData ;
//struct KeyServerData::Incremental;

class KeyServer;
struct GrantKeyServerData;
class GrantKeyServer;
class Date_for_grant;
//struct GrantKeyServerData::Incremental;

#define MIN_GLOBAL_ID 0x1000

struct GrantKeyServerData {

  version_t version;
  int  grant_type;
  string grant_data ;//cryptoed grant key

  GrantKeyServerData()
    : version(0)
    {}

  void encode(bufferlist& bl) const {
     __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(version, bl);
    ::encode(grant_type, bl);
    ::encode(grant_data, bl);        
  }
  void decode(bufferlist::iterator& bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(version, bl);
    ::decode(grant_type, bl); 
    ::decode(grant_data, bl);       
  } 

  typedef enum {
    GRANT_INC_NOP,//maybe not used
    GRANT_INC_ADD, 
  } IncrementalOp;

  struct Incremental {
    IncrementalOp op;   
    int grant_type ;
    string grant_data ;
    
    void encode(bufferlist& bl) const {
      __u8 struct_v = 1;
      ::encode(struct_v, bl);
     __u32 _op = (__u32)op;
      ::encode(_op, bl);    
      ::encode(grant_type, bl); 
      ::encode(grant_data, bl);
    }

    void decode(bufferlist::iterator& bl) {
      __u8 struct_v;
      ::decode(struct_v, bl);
      __u32 _op;
      ::decode(_op, bl);
      op = (IncrementalOp)_op;  
      ::decode(grant_type, bl);       
      ::decode(grant_data, bl);
    }
  };

  void apply_incremental(Incremental& inc) {
    switch (inc.op) {
    case GRANT_INC_ADD:
      add_grant(inc.grant_type, inc.grant_data);
      break;
    case GRANT_INC_NOP:
      break;
    default:
      assert(0);
    }
  }

  void add_grant(int grant_type, string grant_data) {
    this->grant_type = grant_type ;
    this->grant_data = grant_data ;
  }

   void clear_grant() {
    grant_type = 0;
    grant_data = "" ;
  }

};
WRITE_CLASS_ENCODER(GrantKeyServerData)
WRITE_CLASS_ENCODER(GrantKeyServerData::Incremental)


class GrantKeyServer {
  // CephContext *cct;
  mutable Mutex lock;
 
public:
  GrantKeyServer():lock("GrantKeyServer::lock"){}
  GrantKeyServerData data;

  void encode(bufferlist& bl) const {
    ::encode(data, bl);
  }

  void decode(bufferlist::iterator& bl) {
    Mutex::Locker l(lock);
    ::decode(data, bl);
  } 

  version_t get_ver() const {
    Mutex::Locker l(lock);
    return data.version;    
  }  

  void set_ver(version_t ver) {
    Mutex::Locker l(lock);
    data.version = ver;
  }

   void apply_data_incremental(GrantKeyServerData::Incremental& inc) {
    data.apply_incremental(inc);
  }

  void add_grant(int grant_type, string grant_data) {
    Mutex::Locker l(lock);
    data.add_grant(grant_type, grant_data);
  }
  
  void clear_grant() {
    data.clear_grant();
  }

  Mutex& get_lock() const { return lock; }

  bool get_data(){
    if(!data.grant_data.empty()){
      return true;
    }
    return false ;
  }
};
WRITE_CLASS_ENCODER(GrantKeyServer)


class GrantMonitor: public PaxosService {
  enum IncType {
    GLOBAL_ID,// maybe not used
    GRANT_DATA,
  };

public:
  static const int  GRANT_NONE = 0 ;
  static const int  GRANT_TRIAL = 1 ;
  static const int  GRANT_OFFICIAL = 2 ;
  static const int  GRANT_EXPIRE = 3 ;	   

public:
  struct GrantKeyEntity
  {  	
	string  keyring_data ; //the grant key imported 
	string  original_keyring_data ; //after decrypted 

  	string  kvstore_data ; //encrypted  grant key
  	string  original_kvstore_data ;

	int grant_type ;

	string  start_date;//the import date, for verify
  	string  due_date ;//record the expier date of trial 
  	int  official_hosts;
  	int  official_capacity ; 
	string fsid_i;//the import fsid ,for verify
	
  	GrantKeyEntity():
  				due_date("0"),  				
			  	official_hosts(0),
			  	official_capacity(0)
			  	{}
  	GrantKeyEntity(string _keyring_data):
  				keyring_data(_keyring_data),
  				due_date("0"),			  	
			  	official_hosts(0),
			  	official_capacity(0)
  	{}  	
				
  	void parse_from_origin(const string& original_keyring_data1) {
		//fsid/start_date/due_date/0/0TTTTTTTT
		//fsid/0/0/10/1000TTTTTTTT
      char str[500];
      memset(str , '\0', 500);
      memcpy(str,original_keyring_data1.c_str(),original_keyring_data1.size()) ;

      char *start_t;
      char *end_t;
      char *limit_cap ;
      char *limit_nodes;
      
      char *p = NULL;
      p = strtok(str, "/");
      fsid_i = p;
      p = strtok(NULL,"/");
      start_t = p;
      start_date = start_t;
      p = strtok(NULL,"/");
      end_t = p;
      due_date= end_t ;
      p = strtok(NULL,"/");
      limit_nodes = p;
      official_hosts = atoi(limit_nodes);
      p = strtok(NULL, "/");
      limit_cap = p;
		//erase the "T"
      p = strtok(limit_cap, "T");
      limit_cap = p;
      official_capacity = atoi(limit_cap);
      if (start_date[0] == '0') {
        grant_type = GRANT_OFFICIAL ;
        start_date = "0";
        due_date="0";
        official_hosts = atoi(limit_nodes);
        official_capacity = atoi(limit_cap);
      } else {
        grant_type = GRANT_TRIAL ;
        start_date = start_t;
        due_date = end_t;
        official_hosts = 0 ,
        official_capacity = 0 ;
      }
    }

  	bool decode_from_keyring() {
  	  original_keyring_data = Monitor::decrypt_m(Monitor::SKEY_L, keyring_data);
	  if (original_keyring_data == "error")
	    return false;
	  if (original_keyring_data.size() % 16)
	    return false;
  	  parse_from_origin(original_keyring_data) ;
  	  return true;
  	}

  	string encode_to_kv() {
  	  if (!kvstore_data.empty())
  	    return kvstore_data ;
  	  original_kvstore_data = (GRANT_TRIAL == grant_type)?"trial":"official" ;
  	  original_kvstore_data = original_kvstore_data + '/' + due_date  + '/' +
  	    boost::lexical_cast<string>(official_hosts) + '/' + 
  	    boost::lexical_cast<string>(official_capacity) ;
      int64_t sz = original_kvstore_data.size() ;
      if (sz%16) {
        int count = (sz + 15 )/16 ;
        int padding = count * 16 - sz ;
        string padding_str = string(padding, 'T') ;
        original_kvstore_data += padding_str ;
	  }
	  
  	  kvstore_data = Monitor::encrypt_m(Monitor::SKEY_L, original_kvstore_data);
  	  return kvstore_data ;
	}

 	string decode_from_kv() {
	//trial/due_date/0/0TTTTT
	//official/0/10/100TTTTT
 	  original_kvstore_data = Monitor::decrypt_m(Monitor::SKEY_L, kvstore_data) ;
      char *s_lic_type, *end_date, *limit_nodes, *limit_cap;
      char str[500];
      memset(str , '\0', 500);
      memcpy(str,original_kvstore_data.c_str(),original_kvstore_data.size()) ;

      char *p = NULL;
      p = strtok(str, "/");
      s_lic_type = p;

      p = strtok(NULL, "/");
      end_date = p;
      due_date = end_date;

      p = strtok(NULL,"/");
      limit_nodes = p;
      official_hosts = atoi(limit_nodes)  ;

      p = strtok(NULL, "/");
      limit_cap = p;
	  //erase the "T"
	  p = strtok(limit_cap, "T");
      limit_cap = p;
      official_capacity = atoi(limit_cap);
	  
	  string str_type = s_lic_type;
      if (str_type == "trial") {
        grant_type = GRANT_TRIAL ;
      } else {
        grant_type = GRANT_OFFICIAL ;
   	  }
   		return original_kvstore_data ;
	}
  };

  struct Incremental {
    IncType inc_type;
    uint64_t max_global_id;
    int grant_type;
    bufferlist grant_data;

    Incremental() : inc_type(GLOBAL_ID), max_global_id(0){}

    void encode(bufferlist& bl, uint64_t features=-1) const {
      if ((features & CEPH_FEATURE_MONENC) == 0) {
	__u8 v = 1;
	::encode(v, bl);
	__u32 _type = (__u32)inc_type;
	::encode(_type, bl);
	if (_type == GLOBAL_ID) {
	  ::encode(max_global_id, bl);
	} else {
	  ::encode(grant_type, bl);
	  ::encode(grant_data, bl);
	}
	return;
      } 
      ENCODE_START(2, 2, bl);
      __u32 _type = (__u32)inc_type;
      ::encode(_type, bl);
      if (_type == GLOBAL_ID) {
		::encode(max_global_id, bl);
      } else {
		::encode(grant_type, bl);
		::encode(grant_data, bl);
      }
      ENCODE_FINISH(bl);
    }

    void decode(bufferlist::iterator& bl) {
      DECODE_START_LEGACY_COMPAT_LEN(2, 2, 2, bl);
      __u32 _type;
      ::decode(_type, bl);
      inc_type = (IncType)_type;
      assert(inc_type >= GLOBAL_ID && inc_type <= GRANT_DATA);
      if (_type == GLOBAL_ID) {
		::decode(max_global_id, bl);
      } else {
		::decode(grant_type, bl);
		::decode(grant_data, bl);
      }
      DECODE_FINISH(bl);
    }    
 };

private:
  vector<Incremental> pending_grant;
  uint64_t max_global_id;
  uint64_t last_allocated_id;

  void push_grant_inc(GrantKeyServerData::Incremental& grant_inc_data) {
    Incremental inc; 
    inc.inc_type = GRANT_DATA;//the other is GLOBAL_ID
    ::encode(grant_inc_data, inc.grant_data);//grant_data is a bufferlist
    inc.grant_type = grant_inc_data.grant_type;
    pending_grant.push_back(inc);
  }

  void on_active();
  bool should_propose(double& delay);
  void create_initial();
  void update_from_paxos(bool *need_bootstrap);//update GrantKeyServerData version
  void create_pending();  // prepare a new pending
  // propose pending update to peers
  void encode_pending(MonitorDBStore::TransactionRef t);
  void encode_full(MonitorDBStore::TransactionRef t);

  version_t get_trim_to();
  bool preprocess_query(PaxosServiceMessage *m);  // true if processed.
  bool prepare_update(PaxosServiceMessage *m);
  bool prepare_command(MMonCommand *m);
 
 public:
  GrantMonitor(Monitor *mn, Paxos *p, const string& service_name)
    : PaxosService(mn, p, service_name),     
      max_global_id(0),
      last_allocated_id(0)
  {}
  
  void tick(); 
  void update_left_days();  //update the left days
  bool is_grantkey_valid(const string & grantkey, const GrantKeyEntity &grant_key_entity); // check if the grantkey is valid 
  void import_grant_key(GrantKeyEntity& gke) ;
  void dump_info(Formatter *f);
};
WRITE_CLASS_ENCODER_FEATURES(GrantMonitor::Incremental)

#endif
