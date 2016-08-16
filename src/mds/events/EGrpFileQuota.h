

#ifndef CEPH_MDS_EGRP_FILE_QUOTA_H
#define CEPH_MDS_EGRP_FILE_QUOTA_H

#include "../LogEvent.h"
#include "../MDCache.h"
#include "common/GrpFileQuota.h"

class EGrpFileQuota : public LogEvent {
public:
  version_t cmapv;
  string type;
  gid_t gid;
  map<gid_t, GrpFileQuota> update_file_quota_map;
  

  EGrpFileQuota() : LogEvent(EVENT_GRP_FILE_QUOTA), cmapv(0) { }
  EGrpFileQuota(MDLog *mdlog, const char *s) : 
    LogEvent(EVENT_GRP_FILE_QUOTA), 
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
  static void generate_test_instances(list<EGrpFileQuota*>& ls);

  void replay(MDS *mds);
};

#endif

