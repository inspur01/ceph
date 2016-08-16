

#ifndef CEPH_MDS_EUSER_FILE_QUOTA_H
#define CEPH_MDS_EUSER_FILE_QUOTA_H

#include "../LogEvent.h"
#include "../MDCache.h"
#include "common/UserFileQuota.h"

class EUserFileQuota : public LogEvent {
public:
  version_t cmapv;
  string type;
  uid_t uid;
  map<uid_t, UserFileQuota> update_file_quota_map;
  

  EUserFileQuota() : LogEvent(EVENT_USER_FILE_QUOTA), cmapv(0) { }
  EUserFileQuota(MDLog *mdlog, const char *s) : 
    LogEvent(EVENT_USER_FILE_QUOTA), 
    cmapv(0), type(s) { }
    
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
  static void generate_test_instances(list<EUserFileQuota*>& ls);

  void replay(MDS *mds);
};

#endif

