#ifndef CEPH_CLIENTUID_H
#define CEPH_CLIENTUID_H
#include <stdlib.h>
#include <security/pam_appl.h>

class ClientUid{
public:
  int user_authenticate(const char *username, const char *passwd);
  int user_info(const char *username, int *uid, int *gid);
  ClientUid(){};
  ~ClientUid(){};
};

#endif
