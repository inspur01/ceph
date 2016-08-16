#ifndef CEPH_POSIX_ACL
#define CEPH_POSIX_ACL

#include "posix_acl.h"

typedef struct {
  ceph_le16       e_tag;
  ceph_le16       e_perm;
  ceph_le32       e_id;
} acl_ea_entry;

typedef struct {
  ceph_le32       a_version;
  acl_ea_entry    a_entries[0];
} acl_ea_header;

class UserGroups;

int posix_acl_check(const void *xattr, size_t size);
int posix_acl_equiv_mode(const void *xattr, size_t size, mode_t *mode_p);
int posix_acl_inherit_mode(bufferptr& acl, mode_t *mode_p);
int posix_acl_access_chmod(bufferptr& acl, mode_t mode);
int posix_acl_permits(const bufferptr& acl, uid_t i_uid, gid_t i_gid,
		      uid_t uid, UserGroups& groups, unsigned want);
#endif
