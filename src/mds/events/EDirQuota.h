#ifndef CEPH_MDS_EDIRQUOTA_H
#define CEPH_MDS_EDIRQUOTA_H

#include "../LogEvent.h"
#include "../MDCache.h"

class EDirQuota : public LogEvent {
public:
  version_t cmapv;
  string type;
  char op_type;
  DirQuotaData dir_quota_data;
  map<inodeno_t, DirQuotaData> update_quota_map;

  EDirQuota() : LogEvent(EVENT_DIR_QUOTA), cmapv(0) { }
  EDirQuota(MDLog *mdlog, const char *s) : 
    LogEvent(EVENT_DIR_QUOTA), 
    cmapv(0), type(s) { }
    
  void print(ostream& out) const {
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
  static void generate_test_instances(list<EDirQuota*>& ls);

  void replay(MDS *mds);
};

#endif

