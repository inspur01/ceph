
#ifndef __POSIX_ACL_H
#define __POSIX_ACL_H
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/file.h>
#include <time.h>
#include <stdlib.h>
#include <sys/xattr.h>
#include <stdint.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <cassert>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <iostream>
#include <fstream>

using std::set;
using std::map;

class Client;
class Inode;

#ifndef ADD_ACL_141210
#define ADD_ACL_141210
#endif

#define POSIX_ACL_XATTR_ACCESS     "system.posix_acl_access"
#define POSIX_ACL_XATTR_DEFAULT    "system.posix_acl_default"


#define PERM_WALK_CHECK_READ   MAY_READ
#define PERM_WALK_CHECK_WRITE  MAY_WRITE
#define PERM_WALK_CHECK_EXEC   MAY_EXEC
#define PERM_WALK_CHECK_DELETE   MAY_DELETE



#define S_IRWXUGO    (S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO    (S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO        (S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO        (S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO        (S_IXUSR|S_IXGRP|S_IXOTH)


namespace posix
{

#define XATTR_MAX_SIZE 8192


#define MAY_EXEC        0x00000001
#define MAY_WRITE        0x00000002
#define MAY_READ        0x00000004
#define MAY_APPEND        0x00000008
/*用于校验ACL的删除权限*/
#define MAY_DELETE      MAY_ACCESS
#define MAY_ACCESS      0x00000010
#define MAY_OPEN        0x00000020
#define MAY_CHDIR        0x00000040
/* called from RCU mode, don't block */
#define MAY_NOT_BLOCK        0x00000080





#define    EPERM         1    /* Operation not permitted */
#define    ENOENT        2    /* No such file or directory */
#define    ESRCH         3    /* No such process */
#define    EINTR         4    /* Interrupted system call */
#define    EIO           5    /* I/O error */
#define    ENXIO         6    /* No such device or address */
#define    E2BIG         7    /* Argument list too long */
#define    ENOEXEC       8    /* Exec format error */
#define    EBADF         9    /* Bad file number */
#define    ECHILD        10    /* No child processes */
#define    EAGAIN        11    /* Try again */
#define    ENOMEM        12    /* Out of memory */
#define    EACCES        13    /* Permission denied */
#define    EFAULT        14    /* Bad address */
#define    ENOTBLK       15    /* Block device required */
#define    EBUSY         16    /* Device or resource busy */
#define    EEXIST        17    /* File exists */
#define    EXDEV         18    /* Cross-device link */
#define    ENODEV        19    /* No such device */
#define    ENOTDIR       20    /* Not a directory */
#define    EISDIR        21    /* Is a directory */
#define    EINVAL        22    /* Invalid argument */
#define    ENFILE        23    /* File table overflow */
#define    EMFILE        24    /* Too many open files */
#define    ENOTTY        25    /* Not a typewriter */
#define    ETXTBSY       26    /* Text file busy */
#define    EFBIG         27    /* File too large */
#define    ENOSPC        28    /* No space left on device */
#define    ESPIPE        29    /* Illegal seek */
#define    EROFS         30    /* Read-only file system */
#define    EMLINK        31    /* Too many links */
#define    EPIPE         32    /* Broken pipe */
#define    EDOM          33    /* Math argument out of domain of func */
#define    ERANGE        34    /* Math result not representable */

#define UID_GID_MAP_MAX_EXTENTS 5

#define ACL_UNDEFINED_ID    (-1)

/* a_type field in acl_user_posix_entry_t */
#define ACL_TYPE_ACCESS        (0x8000)
#define ACL_TYPE_DEFAULT    (0x4000)

/* e_tag entry in struct posix_acl_entry */
#define ACL_USER_OBJ        (0x01)
#define ACL_USER        (0x02)
#define ACL_GROUP_OBJ        (0x04)
#define ACL_GROUP        (0x08)
#define ACL_MASK        (0x10)
#define ACL_OTHER        (0x20)

/* permissions in the e_perm field */
#define ACL_READ        (0x04)
#define ACL_WRITE        (0x02)
#define ACL_EXECUTE        (0x01)
//#define ACL_ADD        (0x08)
//#define ACL_DELETE        (0x10)



/* Supported ACL a_version fields */
#define POSIX_ACL_XATTR_VERSION    0x0002


#define smp_read_barrier_depends() do { } while(0)

/*
 * The virtio configuration space is defined to be little-endian.  x86 is
 * little-endian too, but it's nice to be explicit so we have these helpers.
 */
#define cpu_to_le16(v16) (v16)
#define cpu_to_le32(v32) (v32)
#define cpu_to_le64(v64) (v64)
//#define le16_to_cpu(v16) (v16)
//#define le32_to_cpu(v32) (v32)
//#define le64_to_cpu(v64) (v64)
typedef unsigned int u32 ;


#define MAX_ERRNO    4095
#define IS_ERR_VALUE(x) ((x) >= (unsigned long)-MAX_ERRNO)


#define __force
typedef unsigned gfp_t;
#define ___GFP_WAIT        0x10u
#define ___GFP_IO        0x40u
#define __GFP_WAIT    ((__force gfp_t)___GFP_WAIT)    /* Can wait and reschedule? */
#define __GFP_IO    ((__force gfp_t)___GFP_IO)    /* Can start physical IO? */
#define GFP_NOIO    (__GFP_WAIT)
#define GFP_NOFS    (__GFP_WAIT | __GFP_IO)

#define kmalloc(a,b) malloc(a)

#define atomic_set(v,i)        ((v)->counter = (i))



#define __bitwise
typedef unsigned short __u16;
typedef unsigned int   __u32;
typedef unsigned long  __u64;
typedef __u16 __bitwise __le16;
typedef __u16 __bitwise __be16;
typedef __u32 __bitwise __le32;
typedef __u32 __bitwise __be32;
typedef __u64 __bitwise __le64;
typedef __u64 __bitwise __be64;

typedef __u16 __bitwise __sum16;
typedef __u32 __bitwise __wsum;



typedef uid_t kuid_t;
typedef gid_t kgid_t;
#define KUIDT_INIT(value) ((kuid_t) value )
#define KGIDT_INIT(value) ((kgid_t) value )
#define INVALID_UID KUIDT_INIT(-1)
#define INVALID_GID KGIDT_INIT(-1)
#define __kuid_val(a) a
#define __kgid_val(a) a



/**
 * struct callback_head - callback structure for use with RCU and task_work
 * @next: next update requests in a list
 * @func: actual update function to call after the grace period.
 */
struct callback_head {
  struct callback_head *next;
  void (*func)(struct callback_head *head);
};
#define rcu_head callback_head



struct uid_gid_map {    /* 64 bytes -- 1 cache line */
  u32 nr_extents;
  struct uid_gid_extent {
    u32 first;
    u32 lower_first;
    u32 count;
  } extent[UID_GID_MAP_MAX_EXTENTS];
};


typedef struct {
  int counter;
} atomic_t;

typedef mode_t umode_t;

struct kref {
  posix::atomic_t refcount;
};

typedef struct _posix_acl_entry {
  short            e_tag;
  unsigned short        e_perm;
  union {
    kuid_t        e_uid;
    kgid_t        e_gid;
#ifndef CONFIG_UIDGID_STRICT_TYPE_CHECKS
    unsigned int    e_id;
#endif
  };
} posix_acl_entry;






typedef struct _posix_acl {
  union {
    posix::atomic_t        a_refcount;
    struct rcu_head        a_rcu;
  };
  unsigned int        a_count;
  posix_acl_entry    a_entries[0];


} posix_acl;

#define FOREACH_ACL_ENTRY(pa, acl, pe) \
    for(pa=(acl)->a_entries, pe=pa+(acl)->a_count; pa<pe; pa++)

typedef struct _inode_cxt {
  kuid_t			  i_uid;
  kgid_t			  i_gid;
}  inode_cxt;

typedef struct {
  __le16            e_tag;
  __le16            e_perm;
  __le32            e_id;
} posix_acl_xattr_entry;

typedef struct {
  __le32            a_version;
  posix_acl_xattr_entry    a_entries[0];
} posix_acl_xattr_header;


class PosixAcl
{
private:
  Client *cli;
  // 用来判断posix_acl_entry中是否有重复的操作
  //map<int, set<int> > mp_pa_id;
  int posix_acl_permission( inode_cxt *ind_cxt,  inode_cxt *env_cxt, const  posix_acl *acl, int want);
  int posix_permission_acl( inode_cxt *ind_cxt,  inode_cxt *env_cxt, const  posix_acl *acl, int want);
  posix_acl * posix_acl_clone(const  posix_acl *acl, gfp_t flags);
  int posix_acl_create_masq(posix_acl *acl, umode_t *mode_p);
  int posix_acl_create(posix_acl **acl, gfp_t gfp, umode_t *mode_p);
  inline size_t posix_acl_xattr_size(int count);
  posix_acl *posix_acl_from_xattr(const void *value, size_t size);
  int posix_acl_to_xattr(const posix_acl *acl,void *buffer, size_t size);
  int permission_walk_ugo(Inode *in, uid_t uid, gid_t gid,int perm_chk, int readlink = 0);
  int fuse_check_acl(Inode *in, const char *acl_xattr, int length, kuid_t uid, kgid_t gid, int mask);
  int is_repeat_id(map<int, set<int> > &mp_pa_id, unsigned int tag, unsigned int id);
  void mp_pa_id_clear(map<int, set<int> > &mp_pa_id);
public:
  PosixAcl(Client *cl):cli(cl) {}
  int permission_walk(Inode *in, uid_t uid, gid_t gid, int perm_chk);
  int permission_walk_parent(Inode *in, uid_t uid, gid_t gid, int perm_chk);
  int fuse_init_acl(Inode *in, umode_t i_mode);
  int fuse_disable_acl_mask(Inode *in);
  int fuse_inherit_acl(Inode *in);
  int fuse_acl_mask_to_groupmode(Inode *in,int uid,int gid);
  int fuse_acl_to_ugo(Inode *in, const char *acl_xattr, int length);
  int fuse_ugo_to_acl(Inode *in, const int mode);
  int check_sticky_parent(struct Inode *in, struct Inode *inChild, uid_t uid);
  bool is_parent_acl(struct Inode *in, uid_t uid, gid_t gid);

};

}


#endif



/*posix_acl.cpp end*/
