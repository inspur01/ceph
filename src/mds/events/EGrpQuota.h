

#ifndef CEPH_MDS_EGRPQUOTA_H
#define CEPH_MDS_EGRPQUOTA_H

#include "../LogEvent.h"
#include "../MDCache.h"

class EGrpQuota : public LogEvent {
public:
  version_t cmapv;
  string type;
  char op_type;  
  GrpQuotaData grp_quota_data;
  map<gid_t,GrpQuotaData> update_quota_map;

  EGrpQuota() : LogEvent(EVENT_GRP_QUOTA), cmapv(0) { }
  EGrpQuota(MDLog *mdlog, const char *s) : 
    LogEvent(EVENT_GRP_QUOTA), 
    cmapv(0), type(s) { }
    
  void print(ostream& out) const 
  {
  }

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
  static void generate_test_instances(list<EGrpQuota*>& ls);

  void replay(MDS *mds);
};

#endif

