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

#include <sstream>

#include "mon/GrantMonitor.h"
#include "mon/Monitor.h"
#include "mon/MonitorDBStore.h"

#include "messages/MMonCommand.h"
#include "messages/MAuth.h"
#include "messages/MAuthReply.h"
#include "messages/MMonGlobalID.h"

#include "common/Timer.h"
#include "common/config.h"
#include "common/cmdparse.h"

#include "osd/osd_types.h"

#include "include/assert.h"
#include "include/str_list.h"

#define dout_subsys ceph_subsys_mon
#undef dout_prefix
#define dout_prefix _prefix(_dout, mon, get_last_committed())
static ostream& _prefix(std::ostream *_dout, Monitor *mon, version_t v) {
  return *_dout << "mon." << mon->name << "@" << mon->rank
    << "(" << mon->get_state_name()
    << ").grant v" << v << " ";
}

ostream& operator<<(ostream& out, GrantMonitor& gm)
{
  return out << "grant";
}

//add by bijingqiangbj@inspur.com
//date:2016-06-11
/*
 Tick function to update the map based on performance every N seconds
*/
// when called ?
void GrantMonitor::tick()
{
  if (!is_active()) return;

  dout(10) << *this << dendl;
  
  if (!mon->is_leader()) return; 
}
/*void GrantMonitor::update_left_days() {
  GrantKeyEntity  gke;
  gke.kvstore_data = mon->grant_key_server->data.grant_data;
  gke.decode_from_kv();
  int64_t left_days_i = atol(gke.kvstore_left_days.c_str())-1;
  
  char buf[16];
  sprintf(buf, "%ld", left_days_i);
  gke.kvstore_left_days = buf;
  gke.encode_to_kv(); 
  import_grant_key(gke);
  propose_pending();
}*/
	
void GrantMonitor::on_active()
{
  dout(10) << "GrantMonitor::on_active()" << dendl;

  if (!mon->is_leader())
    return;
}

void GrantMonitor::create_initial()
{
  dout(10) << "create_initial -- creating initial map" << dendl;

  max_global_id = MIN_GLOBAL_ID;

  Incremental inc;
  inc.inc_type = GLOBAL_ID;
  inc.max_global_id = max_global_id;
  pending_grant.push_back(inc);
  format_version = 1;

  //write default trial grant kering to kvstore ; 
  GrantKeyEntity gke ;
  
  utime_t curr_time = ceph_clock_now(g_ceph_context);
  time_t clu_time = curr_time.tv.tv_sec;
  struct tm *ptm =localtime(&clu_time);
			
  int64_t y_clu = ptm->tm_year + 1900;
  int64_t m_clu = ptm->tm_mon + 1;
  int64_t d_clu = ptm->tm_mday;
  
  string str_d;
  char buf[32];
  sprintf(buf, "%ld%02ld%02ld", y_clu, m_clu, d_clu);
  str_d = buf;
  
  Date_for_grant date_g(str_d);
  date_g.string_to_date();
  date_g.day_to_date(60);
  date_g.date_to_string();
  
  gke.due_date = date_g.date; 

  string original_key = "trial/" + gke.due_date + "/0" + "/0" ;//generate trivial key
  int sz = original_key.size() ;
  if( sz % 16 ){
  	int  count = (sz + 15 )/16 ;
  	int padding = count * 16 - sz ;
  	string padding_str = string(padding, 'T') ;
  	original_key += padding_str ;
  }
  string  kvstore_data = Monitor::encrypt_m(Monitor::SKEY_L, original_key) ;
  dout(10) << "default trial key: " << original_key <<" "<< kvstore_data <<dendl;
  gke.kvstore_data = kvstore_data ;
  
  gke.encode_to_kv();
  import_grant_key(gke);
}

//to update the version of GrantKeyServerData
void GrantMonitor::update_from_paxos(bool *need_bootstrap)
{
  dout(10) << __func__ << dendl;
  version_t version = get_last_committed();
  version_t keys_ver = mon->grant_key_server->get_ver();
  if (version == keys_ver)
    return;
  assert(version >= keys_ver);//to update the version of GrantKeyServerData

  version_t latest_full = get_version_latest_full();

  dout(10) << __func__ << " version " << version << " keys ver " << keys_ver
           << " latest " << latest_full << dendl;

  if ((latest_full > 0) && (latest_full > keys_ver)) {
    bufferlist latest_bl;
    int err = get_version_full(latest_full, latest_bl);
    assert(err == 0);
    assert(latest_bl.length() != 0);
    dout(7) << __func__ << " loading summary e " << latest_full << dendl;
    dout(7) << __func__ << " latest length " << latest_bl.length() << dendl;
    bufferlist::iterator p = latest_bl.begin();
    __u8 struct_v;
    ::decode(struct_v, p);
    ::decode(max_global_id, p);
    ::decode(*(mon->grant_key_server), p);
    mon->grant_key_server->set_ver(latest_full);
    keys_ver = latest_full;
  }

  dout(10) << __func__ << " key server version " << mon->grant_key_server->get_ver() << dendl;

  // walk through incrementals
  while (version > keys_ver) {
    bufferlist bl;
    int ret = get_version(keys_ver+1, bl);
    assert(ret == 0);
    assert(bl.length());

    // reset if we are moving to initial state.  we will normally have
    // keys in here temporarily for bootstrapping that we need to
    // clear out.
    if (keys_ver == 0)
      mon->grant_key_server->clear_grant();

    dout(20) << __func__ << " walking through version " << (keys_ver+1)
             << " len " << bl.length() << dendl;

    bufferlist::iterator p = bl.begin();
    __u8 v;
    ::decode(v, p);
    while (!p.end()) {
      Incremental inc;
      ::decode(inc, p);
      switch (inc.inc_type) {
      case GLOBAL_ID:
        max_global_id = inc.max_global_id;
        break;
      case GRANT_DATA:
        {
          GrantKeyServerData::Incremental grant_inc;
          bufferlist::iterator iter = inc.grant_data.begin();
          ::decode(grant_inc, iter);
          mon->grant_key_server->apply_data_incremental(grant_inc);
          break;
        }
      }
    }

    keys_ver++;
    mon->grant_key_server->set_ver(keys_ver);
  }

  if (last_allocated_id == 0)
    last_allocated_id = max_global_id;

  dout(10) << "update_from_paxos() last_allocated_id=" << last_allocated_id
     << " max_global_id=" << max_global_id
     << " format_version " << format_version
     << dendl;
}

bool GrantMonitor::should_propose(double& delay)
{
  return (!pending_grant.empty());
}

void GrantMonitor::create_pending()
{
  pending_grant.clear();
  dout(10) << "create_pending v " << (get_last_committed() + 1) << dendl;
}

void GrantMonitor::encode_pending(MonitorDBStore::TransactionRef t)
{
  dout(10) << __func__ << " v " << (get_last_committed() + 1) << dendl;

  bufferlist bl;
  __u8 v = 1;
  ::encode(v, bl);
  vector<Incremental>::iterator p;
  for (p = pending_grant.begin(); p != pending_grant.end(); ++p){
    p->encode(bl, mon->get_quorum_features());
  }
  version_t version = get_last_committed() + 1;
  put_version(t, version, bl);
  put_last_committed(t, version);
}

void GrantMonitor::encode_full(MonitorDBStore::TransactionRef t)
{
  version_t version = mon->grant_key_server->get_ver();
  // do not stash full version 0 as it will never be removed nor read
  if (version == 0)
    return;

  dout(10) << __func__ << " grant v " << version << dendl;
  assert(get_last_committed() == version);

  bufferlist full_bl;
  Mutex::Locker l(mon->grant_key_server->get_lock());
  
  __u8 v = 1;
  ::encode(v, full_bl);
  ::encode(max_global_id, full_bl);
  ::encode(*(mon->grant_key_server), full_bl);
  put_version_full(t, version, full_bl);
  put_version_latest_full(t, version);
}

version_t GrantMonitor::get_trim_to()
{
  unsigned max = g_conf->paxos_max_join_drift * 2;
  version_t version = get_last_committed();
  if (mon->is_leader() && (version > max))
    return version - max;
  return 0;
}

//inherited from PaxosService,must implement
//skip this process, use prepare_update only 
bool GrantMonitor::preprocess_query(PaxosServiceMessage *m)
{
  return false;
}

//inherited from PaxosService,must implement
bool GrantMonitor::prepare_update(PaxosServiceMessage *m)
{
  dout(10) << "prepare_update " << *m << " from " << m->get_orig_source_inst() << dendl;
  switch (m->get_type()) {
  case MSG_MON_COMMAND:
    return prepare_command((MMonCommand*)m);
  default:
    assert(0);
    m->put();
    return false;
  }
}

bool GrantMonitor::prepare_command(MMonCommand *m)
{
  stringstream ss;
  bufferlist rdata;
  string rs;
  int err = -EINVAL;
  int r = -1;

  map<string, cmd_vartype> cmdmap;
  if (!cmdmap_from_json(m->cmd, &cmdmap, ss)) {
    // ss has reason for failure
    string rs = ss.str();
    mon->reply_command(m, -EINVAL, rs, rdata, get_last_committed());
    return true;
  }

  string prefix;
  vector<string>caps_vec;
  string keyring_data;

  cmd_getval(g_ceph_context, cmdmap, "prefix", prefix);

  string format;
  cmd_getval(g_ceph_context, cmdmap, "format", format, string("plain"));
  boost::scoped_ptr<Formatter> f(new_formatter(format));

  cmd_getval(g_ceph_context, cmdmap, "grantkey", keyring_data);
  GrantKeyEntity  gke ;
  if(!keyring_data.empty()){
 	gke.keyring_data = keyring_data ;
    if(keyring_data.size() % 16){// check if key is valid first 
     	ss << "grant key is bad!";
    	err = -EINVAL;
    	goto done;
    	}
	}      	

   if (prefix == "grant import" ){ 
   	  if (keyring_data.empty()){
    	ss << "grant key is empty!";
    	err = -EINVAL;
    	goto done;
    }
   	if (!gke.decode_from_keyring())  {
		ss << "grant key is bad!";
		err = -EINVAL;
		goto done;
   		}
   // check if key is valid first 
    if(!is_grantkey_valid(keyring_data, gke)) {
    	ss << "grant key is bad!";
   		err = -EINVAL;
  	    goto done;
		}
	//repeat judge
	string old_kvstore_data;//the old kvstore_data
	old_kvstore_data = mon->grant_key_server->data.grant_data ;
  	if (gke.encode_to_kv() == old_kvstore_data) {
		ss << "grant key is repeated!";
		err = -EINVAL;
		goto done;
  		}
  	//dout(0)<<"keyring_data "<<keyring_data<<" is convented to "<<gke.original_keyring_data << dendl;
  	dout(10)<<"start_date "<<gke.start_date<<dendl;
 	dout(10)<<"due_date "<<gke.due_date<<dendl;
  	dout(10)<<"official_hosts "<<gke.official_hosts<<dendl;
  	dout(10)<<"official_capacity "<<gke.official_capacity<<dendl;

    import_grant_key(gke);
    ss << "import success!\n";
    getline(ss, rs);
    err = 0;
    wait_for_finished_proposal(new Monitor::C_Command(mon, m, 0, rs,
                get_last_committed() + 1));
    return true;

  }else if (prefix == "grant show" ){

     dout(10)<<"before decode_from_kv "<< dendl;
    if(!mon->grant_key_server->get_data()){
      ss<<"don't have grant key!" ;
      r = -ENOENT ;
      goto done ;
    }
    {
      GrantKeyEntity gke ;
      gke.kvstore_data = mon->grant_key_server->data.grant_data ;
      //dout(10)<<"before decode_from_kv, kvstore_data is: "<<gke.kvstore_data<< dendl;
      gke.decode_from_kv() ;      
      //dout(10)<<"decode_from_kv, original_kvstore_data is: "<<gke.original_kvstore_data<< dendl;
       ss<<"due_date="<< gke.due_date << "\n";
       ss<<"official_hosts="<< gke.official_hosts << "\n";
       ss<<"official_capacity="<< gke.official_capacity;
    }
    r = 0 ;
    err = 0;
  }
  done:
    getline(ss, rs,'\0');
    mon->reply_command(m, err, rs, rdata, get_last_committed());
    return true ;
}

bool GrantMonitor::is_grantkey_valid(const string & keyring_data,const GrantKeyEntity &grant_key_entity){
  if(keyring_data.size()%16){
	return false ;	
  }
  // decrypt 
  if (Monitor::decrypt_m(Monitor::SKEY_L, keyring_data) == "error") { 
    return false;
  }
  //match the fsid
  struct uuid_d fsid_get = (mon->monmap)->get_fsid();
  string fsid_clu;
  char buf[40];
  uuid_unparse(fsid_get.uuid, buf);
  fsid_clu = buf;
  dout(10)<<"fsid_clu is "<<fsid_clu<<dendl ;
  dout(10)<<"fsid_i is "<<grant_key_entity.fsid_i<<dendl ;

  if (fsid_clu == grant_key_entity.fsid_i) {
    if (atol(grant_key_entity.start_date.c_str()) != 0) { //is trial
	  utime_t curr_time = ceph_clock_now(g_ceph_context);
	  time_t clu_time = curr_time.tv.tv_sec;
	  struct tm *ptm =localtime(&clu_time);			

	  int64_t y_start = atoi(grant_key_entity.start_date.substr(0,4).c_str());
	  int64_t m_start = atoi(grant_key_entity.start_date.substr(4,2).c_str());
	  int64_t d_start = atoi(grant_key_entity.start_date.substr(6,2).c_str());
	  dout(10)<<"d_start is "<<d_start<<dendl ;

	  int64_t y_clu = ptm->tm_year+1900;
	  int64_t m_clu = ptm->tm_mon+1;
	  int64_t d_clu = ptm->tm_mday;
	  dout(10)<<"d_clu is "<<d_clu<<dendl ;

	  if ((y_clu == y_start) && (m_clu == m_start) && (d_clu == d_start)) {
	    return true; //date is valid
	  } 
	  return false ;//date is not valid
    }
    return true ;// is official
  }
  return false  ;// fsid is not valid
}

void GrantMonitor::import_grant_key(GrantKeyEntity& gke)
{   
//  gke.encode_to_kv();
  //dout(0) << " original_kvstore_data: " << gke.original_kvstore_data << dendl;   
  //convert GrantKeyEntity to GrantKeyServerData::Incremental
  GrantKeyServerData::Incremental grant_inc_data ;
  grant_inc_data.op = GrantKeyServerData::GRANT_INC_ADD ;

  grant_inc_data.grant_data = gke.kvstore_data ;//encrypted  grant key
  grant_inc_data.grant_type = gke.grant_type ;
    
  //dout(0) << " importing keyring_data: " << gke.keyring_data << dendl;   
  //dout(0) << " importing kvstore_data: " << gke.kvstore_data << dendl;   
  push_grant_inc(grant_inc_data);  
}

void GrantMonitor::dump_info(Formatter *f)
{
  /*** WARNING: do not include any privileged information here! ***/
  f->open_object_section("grant");
  f->dump_unsigned("first_committed", get_first_committed());
  f->dump_unsigned("last_committed", get_last_committed());
  f->close_section();
}
