/* Extended attribute names */

#include "posix_acl.h"
#include "Inode.h"
#include "Dentry.h"
#include "Client.h"
#include "Dir.h"


namespace posix
{

#define __must_check
inline void * __must_check ERR_PTR(long error)
{
  return (void *) error;
}

u32 map_id_down(struct uid_gid_map *map, u32 id)
{
  unsigned idx, extents;
  u32 first, last;

  /* Find the matching extent */
  extents = map->nr_extents;
  smp_read_barrier_depends();
  for (idx = 0; idx < extents; idx++) {
    first = map->extent[idx].first;
    last = first + map->extent[idx].count - 1;
    if (id >= first && id <= last)
      break;
  }
  /* Map the id or note failure */
  if (idx < extents)
    id = (id - first) + map->extent[idx].lower_first;
  else
    id = (u32) -1;

  return id;
}

inline kuid_t make_kuid(uid_t uid)
{
  return KUIDT_INIT(uid);
}

inline kgid_t make_kgid(gid_t gid)
{
  return KGIDT_INIT(gid);
}

inline uid_t from_kuid(kuid_t kuid)
{
  return (uid_t)kuid;
}

inline gid_t from_kgid(kgid_t kgid)
{
  return (gid_t)kgid;
}

/*
 * Free an ACL handle.
 */
inline void posix_acl_release(posix_acl *acl)
{
  //if (acl && atomic_dec_and_test(&acl->a_refcount))
  //    kfree_rcu(acl, a_rcu);
  if(acl) free(acl);
}

inline int posix_acl_xattr_count(size_t size)
{
  if (size < sizeof(posix_acl_xattr_header))
    return -1;
  size -= sizeof(posix_acl_xattr_header);
  if (size % sizeof(posix_acl_xattr_entry))
    return -1;
  return size / sizeof(posix_acl_xattr_entry);
}

void posix_acl_init(posix_acl *acl, int count)
{
  atomic_set(&acl->a_refcount, 1);
  acl->a_count = count;
}


posix_acl *posix_acl_alloc(int count, gfp_t flags)
{
  const size_t size = sizeof(posix_acl) + count * sizeof( posix_acl_entry);
  posix_acl *acl = (posix_acl *)kmalloc(size, flags);
  if (acl)
    posix_acl_init(acl, count);
  return acl;
}






inline bool uid_eq(kuid_t left, kuid_t right)
{
  return __kuid_val(left) == __kuid_val(right);
}

inline bool gid_eq(kgid_t left, kgid_t right)
{
  return __kgid_val(left) == __kgid_val(right);
}

inline bool uid_valid(kuid_t uid)
{
  return !uid_eq(uid, INVALID_UID);
}

inline bool gid_valid(kgid_t gid)
{
  return !gid_eq(gid, INVALID_GID);
}

inline void *kmemdup(const void *src, size_t len, gfp_t gfp)
{
  void *p;

  p = malloc(len);
  if (p)
    memcpy(p, src, len);
  return p;
}

/*
 * Clone an ACL.
 */
posix_acl * PosixAcl::posix_acl_clone(const  posix_acl *acl, gfp_t flags)
{
  posix_acl *clone = NULL;

  if (acl) {
    int size = sizeof( posix_acl) + acl->a_count *
               sizeof( posix_acl_entry);
    clone = ( posix_acl *)kmemdup(acl, size, flags);
    if (clone)
      atomic_set(&clone->a_refcount, 1);
  }
  return clone;
}

/*
 * Modify acl when creating a new inode. The caller must ensure the acl is
 * only referenced once.
 *
 * mode_p initially must contain the mode parameter to the open() / creat()
 * system calls. All permissions that are not granted by the acl are removed.
 * The permissions in the acl are changed to reflect the mode_p parameter.
 */
int PosixAcl::posix_acl_create_masq( posix_acl *acl, umode_t *mode_p)
{
  posix_acl_entry *pa, *pe;
  posix_acl_entry *group_obj = NULL, *mask_obj = NULL;
  umode_t mode = *mode_p;
  int not_equiv = 0;

  /* assert(atomic_read(acl->a_refcount) == 1); */

  FOREACH_ACL_ENTRY(pa, acl, pe) {
    switch(pa->e_tag) {
    case ACL_USER_OBJ:
      pa->e_perm &= (mode >> 6) | ~S_IRWXO;
      mode &= (pa->e_perm << 6) | ~S_IRWXU;
      break;

    case ACL_USER:
    case ACL_GROUP:
      not_equiv = 1;
      break;

    case ACL_GROUP_OBJ:
      group_obj = pa;
      break;

    case ACL_OTHER:
      pa->e_perm &= mode | ~S_IRWXO;
      mode &= pa->e_perm | ~S_IRWXO;
      break;

    case ACL_MASK:
      mask_obj = pa;
      not_equiv = 1;
      break;

    default:
      return -EIO;
    }
  }

  if (mask_obj) {
    mask_obj->e_perm &= (mode >> 3) | ~S_IRWXO;
    mode &= (mask_obj->e_perm << 3) | ~S_IRWXG;
  } else {
    if (!group_obj)
      return -EIO;
    group_obj->e_perm &= (mode >> 3) | ~S_IRWXO;
    mode &= (group_obj->e_perm << 3) | ~S_IRWXG;
  }

  *mode_p = (*mode_p & ~S_IRWXUGO) | mode;
  return not_equiv;
}


int PosixAcl::posix_acl_create(posix_acl **acl, gfp_t gfp, umode_t *mode_p)
{
  posix_acl *clone = posix_acl_clone(*acl, gfp);
  int err = -ENOMEM;
  if (clone) {
    err = posix_acl_create_masq(clone, mode_p);
    if (err < 0) {
      posix_acl_release(clone);
      clone = NULL;
    }
  }
  posix_acl_release(*acl);
  *acl = clone;
  return err;
}

inline size_t PosixAcl::posix_acl_xattr_size(int count)
{
  return (sizeof(posix_acl_xattr_header) +
          (count * sizeof(posix_acl_xattr_entry)));
}

/*
 * Convert from extended attribute to in-memory representation.
 */
posix_acl* PosixAcl::posix_acl_from_xattr(const void *value, size_t size)
{
  posix_acl_xattr_header *header = (posix_acl_xattr_header *)value;
  posix_acl_xattr_entry *entry = (posix_acl_xattr_entry *)(header+1), *end;
  int count;
  posix_acl *acl;
  posix_acl_entry *acl_e;

  if (!value)
    return NULL;
  if (size < sizeof(posix_acl_xattr_header))
    return (posix_acl * )ERR_PTR(-EINVAL);
  if (header->a_version != cpu_to_le32(POSIX_ACL_XATTR_VERSION))
    return (posix_acl * )ERR_PTR(-EOPNOTSUPP);

  count = posix_acl_xattr_count(size);
  if (count < 0)
    return (posix_acl * )ERR_PTR(-EINVAL);
  if (count == 0)
    return NULL;

  acl = posix_acl_alloc(count, GFP_NOFS);
  if (!acl)
    return (posix_acl * )ERR_PTR(-ENOMEM);
  acl_e = acl->a_entries;

  for (end = entry + count; entry != end; acl_e++, entry++) {
    acl_e->e_tag  = le16_to_cpu(entry->e_tag);
    acl_e->e_perm = le16_to_cpu(entry->e_perm);

    switch(acl_e->e_tag) {
    case ACL_USER_OBJ:
    case ACL_GROUP_OBJ:
    case ACL_MASK:
    case ACL_OTHER:
      acl_e->e_id = entry->e_id;
      break;

    case ACL_USER:
      acl_e->e_uid =
        make_kuid(le32_to_cpu(entry->e_id));
      if (!uid_valid(acl_e->e_uid))
        goto fail;
      break;
    case ACL_GROUP:
      acl_e->e_gid =
        make_kgid(le32_to_cpu(entry->e_id));
      if (!gid_valid(acl_e->e_gid))
        goto fail;
      break;

    default:
      goto fail;
    }
  }
  return acl;

fail:
  posix_acl_release(acl);
  return (posix_acl * )ERR_PTR(-EINVAL);
}

/*
 * Convert from in-memory to extended attribute representation.
 */
int PosixAcl::posix_acl_to_xattr(const posix_acl *acl,void *buffer, size_t size)
{
  posix_acl_xattr_header *ext_acl = (posix_acl_xattr_header *)buffer;
  posix_acl_xattr_entry *ext_entry = ext_acl->a_entries;
  size_t real_size, n;

  real_size = posix_acl_xattr_size(acl->a_count);
  if (!buffer)
    return real_size;
  if (real_size > size)
    return -ERANGE;

  ext_acl->a_version = cpu_to_le32(POSIX_ACL_XATTR_VERSION);

  for (n=0; n < acl->a_count; n++, ext_entry++) {
    const posix_acl_entry *acl_e = &acl->a_entries[n];
    ext_entry->e_tag  = cpu_to_le16(acl_e->e_tag);
    ext_entry->e_perm = cpu_to_le16(acl_e->e_perm);
    switch(acl_e->e_tag) {
    case ACL_USER:
      ext_entry->e_id =
        cpu_to_le32(from_kuid(acl_e->e_uid));
      break;
    case ACL_GROUP:
      ext_entry->e_id =
        cpu_to_le32(from_kgid(acl_e->e_gid));
      break;
    default:
      ext_entry->e_id = cpu_to_le32(ACL_UNDEFINED_ID);
      break;
    }
  }
  return real_size;
}

/*
 * Keep mostly read-only and often accessed (especially for
 * the RCU path lookup and 'stat' data) fields at the beginning
 * of the 'struct inode_cxt'
 */


#define in_group_p(a)   1

/*
 * Return 0 if current is granted want access to the inode_cxt
 * by the acl. Returns -E... otherwise.
 */
int PosixAcl::posix_acl_permission(inode_cxt *ind_cxt, inode_cxt *env_cxt, const posix_acl *acl, int want)
{
  const posix_acl_entry *pa, *pe, *mask_obj;
  int found = 0;

  want &= MAY_READ | MAY_WRITE | MAY_EXEC | MAY_DELETE | MAY_NOT_BLOCK;

  FOREACH_ACL_ENTRY(pa, acl, pe) {
    switch(pa->e_tag) {
    case ACL_USER_OBJ:
      /* (May have been checked already) */
      if (uid_eq(ind_cxt->i_uid, env_cxt->i_uid))
        goto check_perm;
      break;
    case ACL_USER:
      if (uid_eq(pa->e_uid, env_cxt->i_uid))
        goto mask;
      break;
    case ACL_GROUP_OBJ:
      if (gid_eq(ind_cxt->i_gid, env_cxt->i_gid)) {
        found = 1;
        if ((pa->e_perm & want) == want)
          goto mask;
      }
      break;
    case ACL_GROUP:
      if (gid_eq(pa->e_gid, env_cxt->i_gid)) {
        found = 1;
        if ((pa->e_perm & want) == want)
          goto mask;
      }
      break;
    case ACL_MASK:
      break;
    case ACL_OTHER:
      if (found)
        return -EACCES;
      else
        goto check_perm;
    default:
      return -EIO;
    }
  }
  return -EIO;

mask:
  for (mask_obj = pa+1; mask_obj != pe; mask_obj++) {
    if (mask_obj->e_tag == ACL_MASK) {
      if ((pa->e_perm & mask_obj->e_perm & want) == want)
        return 0;
      return -EACCES;
    }
  }

check_perm:
  if ((pa->e_perm & want) == want)
    return 0;
  return -EACCES;
}

int PosixAcl::posix_permission_acl(inode_cxt *ind_cxt, inode_cxt *env_cxt, const posix_acl *acl, int want)
{
  const posix_acl_entry *pa, *pe, *mask_obj;
  int found = 0;

  want &= MAY_READ | MAY_WRITE | MAY_EXEC | MAY_DELETE | MAY_NOT_BLOCK;

  FOREACH_ACL_ENTRY(pa, acl, pe) {
    switch(pa->e_tag) {
    case ACL_USER_OBJ:
    case ACL_GROUP_OBJ:
    case ACL_MASK:
      break;
    case ACL_USER:
      if (uid_eq(pa->e_uid, env_cxt->i_uid))
        goto mask;
      break;
    case ACL_GROUP:
      if (gid_eq(pa->e_gid, env_cxt->i_gid)) {
        found = 1;
        if ((pa->e_perm & want) == want)
          goto mask;
      }
      break;
    case ACL_OTHER:
        if (found)
        return -EACCES;
    default:
      return -EIO;
    }
  }

  FOREACH_ACL_ENTRY(pa, acl, pe) {
    switch(pa->e_tag) {
    case ACL_USER:
    case ACL_GROUP:
    case ACL_MASK:
        break;
    case ACL_USER_OBJ:
      /* (May have been checked already) */
      if (uid_eq(ind_cxt->i_uid, env_cxt->i_uid))
        goto check_perm;
      break;
    case ACL_GROUP_OBJ:
      if (gid_eq(ind_cxt->i_gid, env_cxt->i_gid)) {
        found = 1;
        if ((pa->e_perm & want) == want)
          goto mask;
      }
      break;
    case ACL_OTHER:
      if (found)
        return -EACCES;
      else
        goto check_perm;
    default:
      return -EIO;
    }
  }
  return -EIO;

mask:
  for (mask_obj = pa+1; mask_obj != pe; mask_obj++) {
    if (mask_obj->e_tag == ACL_MASK) {
      if ((pa->e_perm & mask_obj->e_perm & want) == want)
        return 0;
      return -EACCES;
    }
  }

check_perm:
  if ((pa->e_perm & want) == want)
    return 0;
  return -EACCES;
}

int PosixAcl::permission_walk_ugo(Inode *in, uid_t uid, gid_t gid,int perm_chk, int readlink )
{
  int rt=0;
  //I'm root~~
  if(uid == 0) {
    return 0;
  }
  struct stat stbuf;
  int chk = perm_chk;
  int res = cli->ll_stat(in, &stbuf);
  if(res) {
    rt =  res;
    goto __free_quit;
  }

  int mr,mw,mx;
  if(stbuf.st_uid == uid) {
    mr = S_IRUSR;
    mw = S_IWUSR;
    mx = S_IXUSR;
  } else if(stbuf.st_gid == gid) {
    mr = S_IRGRP;
    mw = S_IWGRP;
    mx = S_IXGRP;
  } else {
    mr = S_IROTH;
    mw = S_IWOTH;
    mx = S_IXOTH;
  }
  if(chk & PERM_WALK_CHECK_READ) {
    if(!(stbuf.st_mode & mr)) {
      rt = -EACCES;
      goto __free_quit;
    }
  }
  if(chk & PERM_WALK_CHECK_WRITE) {
    if(!(stbuf.st_mode & mw)) {
      rt = -EACCES;
      goto __free_quit;
    }
  }
  if(chk & PERM_WALK_CHECK_EXEC) {
    if(!(stbuf.st_mode & mx)) {
      rt = -EACCES;
      goto __free_quit;
    }
  }
  /*add by ylx ACL*/
  if(chk & PERM_WALK_CHECK_DELETE) {
      rt = -EACCES;
      goto __free_quit;
  }

  rt = 0;
__free_quit:
  return rt;
}

int PosixAcl::fuse_check_acl(Inode *in, const char *acl_xattr, int length, kuid_t uid, kgid_t gid, int mask)
{
  int error=-EAGAIN;
  posix_acl *acl;
  acl = posix_acl_from_xattr(acl_xattr, length);
  if(IS_ERR(acl)) {
    error = PTR_ERR(acl);
    printf("error1 = %d\n", error);
    return error;
  }

  inode_cxt inode;
  struct stat stbuf;
  int ret = cli->ll_stat(in, &stbuf);
  if(ret == -1) {
    return -101;
  }
  inode.i_uid = stbuf.st_uid;
  inode.i_gid = stbuf.st_gid;

  inode_cxt evn_cxt;
  evn_cxt.i_uid = uid;
  evn_cxt.i_gid = gid;

  /*modify by yuluxian ACL
   *error = posix_acl_permission(&inode, &evn_cxt, acl, mask);*/
  error = posix_permission_acl(&inode, &evn_cxt, acl, mask);
  posix_acl_release(acl);

  return error;
}


int PosixAcl::permission_walk(Inode *in, uid_t uid, gid_t gid, int perm_chk)
{
  //I'm root~~
  if(uid == 0) {
    return 0;
  }

  char acl_xattr[XATTR_MAX_SIZE];
  memset(acl_xattr, 0x00, sizeof(acl_xattr));

  int length = 0;
  length = cli->ll_getxattr(in, POSIX_ACL_XATTR_ACCESS, acl_xattr, XATTR_MAX_SIZE,getuid(),getgid());
  if(length <= 0) {
    return permission_walk_ugo(in, uid, gid, perm_chk, 0);
  } else {
    return fuse_check_acl(in, acl_xattr, length, uid, gid, perm_chk);
  }
}

int PosixAcl::permission_walk_parent(Inode *in, uid_t uid, gid_t gid, int perm_chk)
{
  /*int l = strlen(path);
  while(--l)
      if(path[l] == '/')
          break;*/
  if(in->ino == 1) {
    return permission_walk(in, uid, gid, perm_chk);
  } else {
    if (in->dn_set.empty())
      return 0;
    if(in->get_first_parent()->dir == NULL)
      return 0;
    return permission_walk(/*in->get_first_parent()->inode*/in->get_first_parent()->dir->parent_inode, uid, gid, perm_chk);
  }
}



int PosixAcl::fuse_init_acl(Inode *in, umode_t i_mode)
{
  int error=-EAGAIN;
  char acl_xattr[XATTR_MAX_SIZE];
  memset(acl_xattr, 0x00, sizeof(acl_xattr));
  struct stat stbuf;
  int length = 0;

  if (in->dn_set.empty())
    return 0;

  length = cli->ll_getxattr(/*in->get_first_parent()->inode*/in->get_first_parent()->dir->parent_inode, POSIX_ACL_XATTR_DEFAULT, acl_xattr, XATTR_MAX_SIZE,getuid(),getgid());
  if(length <= 0) {
    return 0;
  }

  posix_acl *acl;
  acl = posix_acl_from_xattr(acl_xattr, length);
  if(IS_ERR(acl)) {
    error = PTR_ERR(acl);
    printf("error1 = %d\n", error);
    return error;
  }

  if (acl) {
    char buffer[XATTR_MAX_SIZE];

    if (S_ISDIR(i_mode)) {
      memset(buffer, 0x00, sizeof(buffer));
      int real_len = posix_acl_to_xattr(acl, buffer, XATTR_MAX_SIZE);
      error = cli->ll_setxattr(in, POSIX_ACL_XATTR_DEFAULT, buffer, real_len, 0,getuid(),getgid());
      if (error)
        goto cleanup;
    }
    error = posix_acl_create(&acl, GFP_NOFS, &i_mode);
    if (error < 0)
      return error;

    if (error > 0) {
      /* This is an extended ACL */
      //error = ext4_set_acl(handle, inode, ACL_TYPE_ACCESS, acl);
      memset(buffer, 0x00, sizeof(buffer));
      int real_len = posix_acl_to_xattr(acl, buffer, XATTR_MAX_SIZE);
      error = cli->ll_setxattr(in, POSIX_ACL_XATTR_ACCESS, buffer, real_len, 0,getuid(),getgid());
      if (error)
        goto cleanup;
      cli->ll_chmod(in,i_mode);
    }
  }
cleanup:
  posix_acl_release(acl);
  return error;
}
int PosixAcl::fuse_disable_acl_mask(Inode *in)
{
  int error=-EAGAIN;

  char acl_xattr[XATTR_MAX_SIZE];
  memset(acl_xattr, 0x00, sizeof(acl_xattr));

  //int length = ceph_getxattr(cmount, path, POSIX_ACL_XATTR_ACCESS, acl_xattr, XATTR_MAX_SIZE);
  int length = cli->ll_getxattr(in, POSIX_ACL_XATTR_ACCESS, acl_xattr, XATTR_MAX_SIZE,getuid(),getgid());
  if(length <= 0) {
    return 0;
  }

  posix_acl *acl;
  acl = posix_acl_from_xattr(acl_xattr, length);
  if(IS_ERR(acl)) {
    error = PTR_ERR(acl);
    return error;
  }

  if (acl) {
 
    posix_acl_entry *pa, *pe;
    FOREACH_ACL_ENTRY(pa, acl, pe) {
      switch(pa->e_tag) {
      case ACL_USER_OBJ:
      case ACL_USER:
      case ACL_GROUP_OBJ:
      case ACL_GROUP:
      case ACL_OTHER:
        break;
      case ACL_MASK:
        pa->e_perm = 7;
        break;
      default:
        break;
      }
    }

    char buffer[XATTR_MAX_SIZE];
    memset(buffer, 0x00, sizeof(buffer));

    int real_len = posix_acl_to_xattr(acl, buffer, XATTR_MAX_SIZE);
    //error = ceph_setxattr(cmount, path, POSIX_ACL_XATTR_ACCESS, buffer, real_len, 0);
    error = cli->ll_setxattr(in, POSIX_ACL_XATTR_ACCESS, buffer, real_len, 0,getuid(),getgid());
    if (error) goto cleanup;
  }
cleanup:
  posix_acl_release(acl);
  return error;
}
int PosixAcl::fuse_inherit_acl(Inode *in)
{
  int error=-EAGAIN;

  /*èŽ·å–çˆ¶ç›®å½•çš„default ACLä¿¡æ¯*/
  // int l = strlen(path);
  // while(--l)
  //     if(path[l] == '/')
  //         break;

  char acl_xattr[XATTR_MAX_SIZE];
  memset(acl_xattr, 0x00, sizeof(acl_xattr));

  //int length = ceph_getxattr(cmount, std::string(path, l).c_str(), POSIX_ACL_XATTR_ACCESS, acl_xattr, XATTR_MAX_SIZE);
  int length = cli->ll_getxattr(in->get_first_parent()->inode,POSIX_ACL_XATTR_ACCESS, acl_xattr, XATTR_MAX_SIZE,getuid(),getgid());
  if(length <= 0) {
    return 0;
  }

  posix_acl *acl;
  acl = posix_acl_from_xattr(acl_xattr, length);
  if(IS_ERR(acl)) {
    error = PTR_ERR(acl);
    return error;
  }

  /*æ ¹æ®çˆ¶ç›®å½•çš„default ACLä¿¡æ¯ç”Ÿæˆå­ç›®å½•æˆ–æ–‡ä»¶çš„ACLä¿¡æ¯*/
  if (acl) {
    char buffer[XATTR_MAX_SIZE];
    memset(buffer, 0x00, sizeof(buffer));

    int real_len = posix_acl_to_xattr(acl, buffer, XATTR_MAX_SIZE);
    //error = ceph_setxattr(cmount, path, POSIX_ACL_XATTR_ACCESS, buffer, real_len, 0);
    error = cli->ll_setxattr(in, POSIX_ACL_XATTR_ACCESS, buffer, real_len, 0,getuid(),getgid());
    if (error)
      goto cleanup;
  }
cleanup:
  posix_acl_release(acl);
  return error;
}
int PosixAcl::fuse_acl_mask_to_groupmode(Inode *in,int uid,int gid)
{
  int group_mode = 0;

  char acl_xattr[XATTR_MAX_SIZE];
  memset(acl_xattr, 0x00, sizeof(acl_xattr));

  int length = cli->ll_getxattr(in, POSIX_ACL_XATTR_ACCESS, acl_xattr, XATTR_MAX_SIZE,uid,gid);
  if(length <= 0) {
    return -1;
  }

  posix_acl *acl;
  acl = posix_acl_from_xattr(acl_xattr, length);
  if(IS_ERR(acl)) {
    return -2;
  }

  if (acl) {
    posix_acl_entry *pa, *pe;

    FOREACH_ACL_ENTRY(pa, acl, pe) {
      switch(pa->e_tag) {
      case ACL_USER_OBJ:
        break;
      case ACL_USER:
        break;
      case ACL_GROUP_OBJ:
        break;
      case ACL_GROUP:
        break;
      case ACL_OTHER:
        break;
      case ACL_MASK:
        group_mode= pa->e_perm;
        break;
      default:
        break;
      }
    }
  }

  posix_acl_release(acl);
  return group_mode;
}

int PosixAcl::is_repeat_id(map<int, set<int> > &mp_pa_id, unsigned int tag, unsigned int id)
{
    map<int, set<int> >::iterator it = mp_pa_id.find(tag);
    if (it != mp_pa_id.end())
    {
        set<int> set_id = it->second;
        set<int>::iterator iter = set_id.find(id);
        if (iter != set_id.end())
        {
            return 1;
        }

        set_id.insert(id);
        return 0;
    }
    
    mp_pa_id[tag].insert(id);
    return 0;
}

void PosixAcl::mp_pa_id_clear(map<int, set<int> > &mp_pa_id)
{
    map<int, set<int> >::iterator it = mp_pa_id.begin();
    for (int i = 0; it != mp_pa_id.end(); it++, i++)
    {
        mp_pa_id[i].clear();
    }
    
    mp_pa_id.clear();
}
 
int PosixAcl::fuse_acl_to_ugo(Inode *in, const char *acl_xattr, int length)
{
  int mode = 0;
  int acl_count = 0;
  struct stat stbuf;
  if(in) {
    cli->ll_stat(in,&stbuf);
    mode|=(stbuf.st_mode&(~0x1FF));
  }
  
  posix_acl *acl;
  acl = posix_acl_from_xattr(acl_xattr, length);
  if(IS_ERR(acl)) {
    return -2;
  }

  if (acl) {
    posix_acl_entry *pa, *pe;
    // ÓÃÀ´ÅÐ¶Ïposix_acl_entryÖÐÊÇ·ñÓÐÖØ¸´µÄ²Ù×÷
    map<int, set<int> > mp_pa_id;
    
    FOREACH_ACL_ENTRY(pa, acl, pe) {
      switch(pa->e_tag) {
      case ACL_USER_OBJ:
      case ACL_USER:
      case ACL_GROUP_OBJ:
      case ACL_GROUP:
      case ACL_OTHER:
      case ACL_MASK:
      default:
        if (is_repeat_id(mp_pa_id, pa->e_tag, pa->e_id))
        {
            mp_pa_id.clear();
            return -EINVAL;
        }
        acl_count++;
        break;
      }
    }

    mp_pa_id.clear();

    if(acl_count == 3) {
      char acl_xattr_old[XATTR_MAX_SIZE];
      memset(acl_xattr_old, 0x00, sizeof(acl_xattr_old));

      int length_old = cli->ll_getxattr(in, POSIX_ACL_XATTR_ACCESS, acl_xattr_old, XATTR_MAX_SIZE, getuid(),getgid());
      if(length_old > 0) {
        cli->ll_removexattr(in, POSIX_ACL_XATTR_ACCESS, getuid(), getgid());
        posix_acl_release(acl);
        return 0;
      } else {
        FOREACH_ACL_ENTRY(pa, acl, pe) {
          switch(pa->e_tag) {
          case ACL_USER_OBJ:
            if(pa->e_perm & 4) mode |= S_IRUSR;
            if(pa->e_perm & 2) mode |= S_IWUSR;
            if(pa->e_perm & 1) mode |= S_IXUSR;
            break;
          case ACL_GROUP_OBJ:
            if(pa->e_perm & 4) mode |= S_IRGRP;
            if(pa->e_perm & 2) mode |= S_IWGRP;
            if(pa->e_perm & 1) mode |= S_IXGRP;
            break;
          case ACL_OTHER:
            if(pa->e_perm & 4) mode |= S_IROTH;
            if(pa->e_perm & 2) mode |= S_IWOTH;
            if(pa->e_perm & 1) mode |= S_IXOTH;
            break;
          default:
            break;
          }
        }
        cli->ll_chmod(in,mode);
        posix_acl_release(acl);
        return 0;
      }
    } else {
      bool bMask = false;
      unsigned short mask = 0;
      FOREACH_ACL_ENTRY(pa, acl, pe) {
        switch(pa->e_tag) {
        case ACL_USER_OBJ:
          if(pa->e_perm & 4) mode |= S_IRUSR;
          if(pa->e_perm & 2) mode |= S_IWUSR;
          if(pa->e_perm & 1) mode |= S_IXUSR;
          break;
        case ACL_GROUP_OBJ:
          if(pa->e_perm & 4) mode |= S_IRGRP;
          if(pa->e_perm & 2) mode |= S_IWGRP;
          if(pa->e_perm & 1) mode |= S_IXGRP;
          break;
        case ACL_OTHER:
          if(pa->e_perm & 4) mode |= S_IROTH;
          if(pa->e_perm & 2) mode |= S_IWOTH;
          if(pa->e_perm & 1) mode |= S_IXOTH;
          break;
        //add by lvq
        case ACL_MASK:
          bMask = true;
          mask = pa->e_perm;
        default:
          break;
        }
      }

      //add by lvq
      if (bMask)
      {
          mode = ((mode & 0777707) | ((mask & S_IRWXO) << 3));
      }
      cli->ll_chmod(in,mode);
    }
  }

  posix_acl_release(acl);
  return 1;
}
int PosixAcl::fuse_ugo_to_acl(Inode *in, const int mode)
{
  int new_mode = mode;
  char acl_xattr[XATTR_MAX_SIZE];
  memset(acl_xattr, 0x00, sizeof(acl_xattr));

  int length = cli->ll_getxattr(in,POSIX_ACL_XATTR_ACCESS, acl_xattr, XATTR_MAX_SIZE,getuid(),getgid());
  if(length <= 0) {
    return -1;
  }

  posix_acl *acl;
  acl = posix_acl_from_xattr(acl_xattr, length);
  if(IS_ERR(acl)) {
    return -2;
  }

  if (acl) {
    posix_acl_entry *pa, *pe;

    FOREACH_ACL_ENTRY(pa, acl, pe) {
      switch(pa->e_tag) {
      case ACL_USER_OBJ:
        pa->e_perm = 0;
        if(mode & S_IRUSR) pa->e_perm+= 4;
        if(mode & S_IWUSR) pa->e_perm+= 2;
        if(mode & S_IXUSR) pa->e_perm+= 1;
        break;
      case ACL_OTHER:
        pa->e_perm = 0;
        if(mode & S_IROTH) pa->e_perm+= 4;
        if(mode & S_IWOTH) pa->e_perm+= 2;
        if(mode & S_IXOTH) pa->e_perm+= 1;
        break;
      case ACL_MASK:
        pa->e_perm = 0;
        if(mode & S_IRGRP) pa->e_perm+= 4;
        if(mode & S_IWGRP) pa->e_perm+= 2;
        if(mode & S_IXGRP) pa->e_perm+= 1;
        break;
      case ACL_GROUP_OBJ:
        if(new_mode & S_IRGRP) new_mode -= S_IRGRP;
        if(new_mode & S_IWGRP) new_mode -= S_IWGRP;
        if(new_mode & S_IXGRP) new_mode -= S_IXGRP;

        if(pa->e_perm & 4) new_mode |= S_IRGRP;
        if(pa->e_perm & 2) new_mode |= S_IWGRP;
        if(pa->e_perm & 1) new_mode |= S_IXGRP;
        break;
      default:
        break;
      }
    }

    //cli->ll_chmod(in, new_mode);

    char buffer[XATTR_MAX_SIZE];
    memset(buffer, 0x00, sizeof(buffer));

    int real_len = posix_acl_to_xattr(acl, buffer, XATTR_MAX_SIZE);
    cli->ll_setxattr(in, POSIX_ACL_XATTR_ACCESS, buffer, real_len, 0,getuid(),getgid());
  }

  posix::posix_acl_release(acl);
  return 0;

}
int PosixAcl::check_sticky_parent(struct Inode *in, struct Inode *inChild, uid_t uid)
{
  //I'm root~~
  if(uid == 0) {
    return 0;
  }


  if (!(in->mode & S_ISVTX))
    return 0;

  if (in->uid == uid)
    return 0;

  if (inChild->uid == uid)
    return 0;

  return 1;
}

bool PosixAcl::is_parent_acl(struct Inode *in, uid_t uid, gid_t gid)
{
    char acl_xattr[XATTR_MAX_SIZE];
    memset(acl_xattr, 0x00, sizeof(acl_xattr));
    int length = 0;

    length = cli->ll_getxattr(in, POSIX_ACL_XATTR_DEFAULT, acl_xattr, XATTR_MAX_SIZE, uid, gid);
    if (length > 0)
    {
        return true;
    }

    return false;
}

};
