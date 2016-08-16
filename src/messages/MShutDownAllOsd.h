/*
 * add by zhanghao
 */


#ifndef CEPH_MSHUTDOWNALLOSD_H   
#define CEPH_MSHUTDOWNALLOSD_H
 
#include "msg/Message.h"
#include "include/types.h"
#include "mon/mon_types.h"


class MShutDownAllOsd : public Message {
public:
	bool is_shutdown_all;
    MShutDownAllOsd() : Message(CEPH_MSG_SHUT_DOWN_ALL_OSD) {}
	MShutDownAllOsd(bool& flag) : Message(CEPH_MSG_SHUT_DOWN_ALL_OSD), is_shutdown_all(flag) {}
private:
    ~MShutDownAllOsd() {}
  
public:
  const char *get_type_name() const { return "mon_shutdown_all_osd"; }
  void print(ostream& out) const {
  out << "the shutdown flag is (" << is_shutdown_all << ")";
  }
  
  void encode_payload(uint64_t features) {
  ::encode(is_shutdown_all, payload);
  }
    void decode_payload() {
  bufferlist::iterator p = payload.begin();
  ::decode(is_shutdown_all, p);
  }

};
#endif                         