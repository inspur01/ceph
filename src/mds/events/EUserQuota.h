

#ifndef CEPH_MDS_EUSERQUOTA_H
#define CEPH_MDS_EUSERQUOTA_H

#include "../LogEvent.h"
#include "../MDCache.h"

class EUserQuota : public LogEvent {
public:
  version_t cmapv;
  string type;
  char op_type; 
  uint64_t  old_hardlimit;
  UserQuotaData user_quota_data;
  map<uid_t, UserQuotaData> update_quota_map;

  EUserQuota() : LogEvent(EVENT_USER_QUOTA), cmapv(0), old_hardlimit(0) { }
  EUserQuota(MDLog *mdlog, const char *s) : 
    LogEvent(EVENT_USER_QUOTA), 
    cmapv(0), type(s), old_hardlimit(0) { }
    
  void print(ostream& out) const { }

  void set_version(version_t v) 
  {
	cmapv = v;
  }

  version_t get_version() 
  {
	return cmapv;
  }
  
  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator& bl);
  void dump(Formatter *f) const;
  static void generate_test_instances(list<EUserQuota*>& ls);

  void user_quota_replay(MDS *mds);
  void grp_used_quota_replay(MDS *mds);

  void replay(MDS *mds);
};

#endif

