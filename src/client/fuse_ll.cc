// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#define FUSE_USE_VERSION 30

#include <fuse/fuse.h>
#include <fuse/fuse_lowlevel.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

// ceph
#include "Inode.h"
#include "common/errno.h"
#include "common/safe_io.h"
#include "include/types.h"
#include "Client.h"
#include "Fh.h"
#include "ioctl.h"
#include "common/config.h"
#include "include/assert.h"
#include "ceph_fuse_opt.h"

#include "fuse_ll.h"

#define FINO_INO(x) ((x) & ((1ull<<48)-1ull))
#define FINO_STAG(x) ((x) >> 48)
#define MAKE_FINO(i,s) ((i) | ((s) << 48))

#define MINORBITS	20
#define MINORMASK	((1U << MINORBITS) - 1)

#define MAJOR(dev)	((unsigned int) ((dev) >> MINORBITS))
#define MINOR(dev)	((unsigned int) ((dev) & MINORMASK))
#define MKDEV(ma,mi)	(((ma) << MINORBITS) | (mi))

// add by li.jiebj
#define DFTPERM (cfuse->client->cct->_conf->fuse_default_permissions)
#define DFTPERM_ACL (cfuse->client->cct->_conf->fuse_check_acl_permissions)
#define ERR_NUM(a) (a<0?-1*a:a)

#undef dout_subsys
#define dout_subsys ceph_subsys_client
#undef dout_prefix
#define dout_prefix *_dout << "fuse_ll."

#define ACL_MAX_SIZE 4056
#define ACL_CIFS_MAX_SIZE 4036


std::string get_create_mode(mode_t flags)
{
  std::string mode = std::string("(");

  mode += std::string(" u:");
  if (flags & S_IRUSR)
    mode += std::string("r");
  else mode += std::string("-");
  if (flags & S_IWUSR)
    mode += std::string("w");
  else mode += std::string("-");
  if (flags & S_IXUSR)
    mode += std::string("x");
  else mode += std::string("-");

  mode += std::string(" g:");
  if (flags & S_IRGRP)
    mode += std::string("r");
  else mode += std::string("-");
  if (flags & S_IWGRP)
    mode += std::string("w");
  else mode += std::string("-");
  if (flags & S_IXGRP)
    mode += std::string("x");
  else mode += std::string("-");

  mode += std::string(" o:");
  if (flags & S_IROTH)
    mode += std::string("r");
  else mode += std::string("-");
  if (flags & S_IWOTH)
    mode += std::string("w");
  else mode += std::string("-");
  if (flags & S_IXOTH)
    mode += std::string("x");
  else mode += std::string("-");

  mode += std::string(")");
  return mode;
}

std::string get_open_mode(int flags)
{
  std::string mode = std::string("(");

  switch (flags & O_ACCMODE) {
  case O_WRONLY:
    mode += std::string("O_WRONLY");
    break;
  case O_RDONLY:
    mode += std::string("O_RDONLY");
    break;
  case O_RDWR:
    mode += std::string("O_RDWR");
    break;
  case O_ACCMODE: /* this is what the VFS does */
    mode += std::string("O_RDONLY");
    break;
  }

  if (flags & O_EXCL)
    mode += std::string("|O_EXCL");
  if (flags & O_APPEND)
    mode += std::string("|O_APPEND");
  if (flags & O_CREAT)
    mode += std::string("|O_CREAT");
  if (flags & O_NONBLOCK)
    mode += std::string("|O_NONBLOCK");
  if (flags & O_DIRECT)
    mode += std::string("|O_DIRECT");
  if (flags & O_SYNC)
    mode += std::string("|O_SYNC");
  mode += std::string(")");
  return mode;
}

//add by li.jiebj

static uint32_t new_encode_dev(dev_t dev)
{
  unsigned major = MAJOR(dev);
  unsigned minor = MINOR(dev);
  return (minor & 0xff) | (major << 8) | ((minor & ~0xff) << 12);
}

static dev_t new_decode_dev(uint32_t dev)
{
  unsigned major = (dev & 0xfff00) >> 8;
  unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);
  return MKDEV(major, minor);
}

class CephFuse::Handle
{
public:
  Handle(Client *c, int fd);
  ~Handle();

  int init(int argc, const char *argv[]);
  int start();
  int loop();
  void finalize();

  uint64_t fino_snap(uint64_t fino);
  vinodeno_t fino_vino(inodeno_t fino);
  uint64_t make_fake_ino(inodeno_t ino, snapid_t snapid);
  Inode * iget(inodeno_t fino);
  void iput(Inode *in);

  int fd_on_success;
  Client *client;

  struct fuse_chan *ch;
  struct fuse_session *se;
  char *mountpoint;

  Mutex stag_lock;
  int last_stag;

  ceph::unordered_map<uint64_t,int> snap_stag_map;
  ceph::unordered_map<int,uint64_t> stag_snap_map;

  struct fuse_args args;
};

static void fuse_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  struct fuse_entry_param fe;
  Inode *i2, *i1 = cfuse->iget(parent); // see below
  int r;

  memset(&fe, 0, sizeof(fe));
  r = cfuse->client->ll_lookup(i1, name, &fe.attr, &i2, ctx->uid, ctx->gid);
  if (r >= 0) {
    fe.ino = cfuse->make_fake_ino(fe.attr.st_ino, fe.attr.st_dev);
    fe.attr.st_rdev = new_encode_dev(fe.attr.st_rdev);
    fuse_reply_entry(req, &fe);
  } else {
    fuse_reply_err(req, -r);
  }

  // XXX NB, we dont iput(i2) because FUSE will do so in a matching
  // fuse_ll_forget()
  cfuse->iput(i1);
}

static void fuse_ll_forget(fuse_req_t req, fuse_ino_t ino,
                           long unsigned nlookup)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  cfuse->client->ll_forget(cfuse->iget(ino), nlookup+1);
  fuse_reply_none(req);
}

static void fuse_ll_getattr(fuse_req_t req, fuse_ino_t ino,
                            struct fuse_file_info *fi)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(ino);
  struct stat stbuf;

  (void) fi; // XXX
  if(!DFTPERM) {
    int ret = cfuse->client->ll_permission_walk_parent(in, ctx->uid, ctx->gid,PERM_WALK_CHECK_EXEC);
    if(ret) {
      fuse_reply_err(req, ERR_NUM(ret));
      goto LAST;
    }
  }
  if (cfuse->client->ll_getattr(in, &stbuf, ctx->uid, ctx->gid) == 0) {
    stbuf.st_ino = cfuse->make_fake_ino(stbuf.st_ino, stbuf.st_dev);
    stbuf.st_rdev = new_encode_dev(stbuf.st_rdev);
    if(!DFTPERM) {
      int group_mode = cfuse->client->ll_acl_mask_to_groupmode(in,ctx->uid,ctx->gid);

      if(group_mode >= 0) {

        if(group_mode & 4) stbuf.st_mode |= S_IRGRP;
        if(group_mode & 2) stbuf.st_mode |= S_IWGRP;
        if(group_mode & 1) stbuf.st_mode |= S_IXGRP;

      }
    }
    fuse_reply_attr(req, &stbuf, 0);
  } else
    fuse_reply_err(req, ENOENT);
LAST:
  cfuse->iput(in); // iput required
}

int is_inode_change_ok(const Inode *in, const struct fuse_ctx *ctx, struct stat *attr, int to_set)
{
    //I'm root~~
    if(ctx->uid == 0)
    {
        return 0;
    }

    int retval = -EPERM;

    dout(9)<<"is_inode_change_ok attr->st_mode dec:"<<attr->st_mode <<" oct:" << oct << attr->st_mode
           << dec << " ctx->uid : " << ctx->uid  << " ctx->gid : " << ctx->gid
           << " attr->st_uid : " << attr->st_uid  << " attr->st_gid : " << attr->st_gid
           << " in->uid : " << in->uid  << " in->gid : " << in->gid
           << oct << " to_set : " << to_set << dec <<dendl;
    // 如果原来的uid是root，不允许其他用户改
    /* Make sure a caller can chown. */
    if ((to_set & FUSE_SET_ATTR_UID) && (ctx->uid != in->uid || attr->st_uid != in->uid))
    {
        if (attr->st_uid != 0 || in->uid == 0)
            goto error;
    }

    /* Make sure caller can chgrp. */
    //这里得不到完整的groups，无法进行有效判断，所以部分用例无法解决
    // 非超级用户不能改超级用户创建的目录或文件
    if ((to_set & FUSE_SET_ATTR_GID) && (ctx->gid != in->gid || attr->st_gid != in->gid))
    {
        if (ctx->gid != 0 && in->gid == 0)
            goto error;
    }

    /* Make sure a caller can chmod. */
    if (to_set & FUSE_SET_ATTR_MODE)
    {
        if (ctx->uid != in->uid)
            goto error;

        int gid = (to_set & FUSE_SET_ATTR_GID) ? attr->st_gid : in->gid;
        if (gid != in->gid)
        {
            attr->st_mode &= ~S_ISGID;
        }
    }

    /* Check for setting the inode time. */
    if (to_set & (FUSE_SET_ATTR_ATIME_NOW | FUSE_SET_ATTR_MTIME_NOW))
    {
        if (ctx->uid != in->uid)
            goto error;
    }
fine:
    retval = 0;
error:
    dout(9)<<"is_inode_change_ok attr->st_mode dec:"<<attr->st_mode <<" oct:" << oct << attr->st_mode
           << dec << " retval : " << retval  << dendl;
    return retval;
}

static void fuse_ll_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                            int to_set, struct fuse_file_info *fi)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(ino);
  int mask = 0;
  int r = 0;
  if(!DFTPERM) {
    r = cfuse->client->ll_permission_walk_parent(in, ctx->uid, ctx->gid, PERM_WALK_CHECK_EXEC);
    if(r)
    {
        fuse_reply_err(req, -r);
        goto LAST;
    }

    r = is_inode_change_ok(in, ctx, attr, to_set);
    if (r)
    {
        fuse_reply_err(req, -r);
        goto LAST;
    }

    dout(1)<<"fuse_ll_setattr to_set: "<< to_set << " O_RDWR:" << O_RDWR  <<dendl;
    if(to_set & O_RDWR)
    {
        r = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid, PERM_WALK_CHECK_READ|PERM_WALK_CHECK_WRITE);
    }
    else if (to_set & O_TRUNC || to_set & O_APPEND || to_set & FUSE_SET_ATTR_SIZE)
    {
        r = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid, PERM_WALK_CHECK_WRITE);
    }

    if(r)
    {
        fuse_reply_err(req, -r);
        goto LAST;
    }
    
    struct stat stbuf;
    r = cfuse->client->ll_stat(in, &stbuf);
    if(r) {
      fuse_reply_err(req, ERR_NUM(r));
      goto LAST;
    }
  }

  if (to_set & FUSE_SET_ATTR_MODE) mask |= CEPH_SETATTR_MODE;
  if (to_set & FUSE_SET_ATTR_UID) mask |= CEPH_SETATTR_UID;
  if (to_set & FUSE_SET_ATTR_GID) mask |= CEPH_SETATTR_GID;
  if (to_set & FUSE_SET_ATTR_MTIME) mask |= CEPH_SETATTR_MTIME;
  if (to_set & FUSE_SET_ATTR_ATIME) mask |= CEPH_SETATTR_ATIME;
  if (to_set & FUSE_SET_ATTR_SIZE) mask |= CEPH_SETATTR_SIZE;

  if(!DFTPERM) {
    if(mask & CEPH_SETATTR_MODE)
      cfuse->client->ll_ugo_to_acl(in, attr->st_mode);
  }
  r = cfuse->client->ll_setattr(in, attr, mask, ctx->uid, ctx->gid);
  if (r == 0)
    fuse_reply_attr(req, attr, 0);
  else
    fuse_reply_err(req, -r);
LAST:
  cfuse->iput(in); // iput required
}

// XATTRS

static void fuse_ll_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                             const char *value, size_t size, int flags)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(ino);
  struct stat stbuf;
  int r = 0;
  if(!DFTPERM) {
    r = cfuse->client->ll_permission_walk_parent(in, ctx->uid, ctx->gid, PERM_WALK_CHECK_EXEC);
    if(r)
    {
        goto LAST;
    }

    /* permission check*/
    r = cfuse->client->ll_stat(in, &stbuf);
    if(r) {
      goto LAST;
    }
    
    if((ctx->uid != stbuf.st_uid) && (ctx->uid != 0)) {
      r = -EPERM;

      goto LAST;
    }    

    if(strcmp(name, POSIX_ACL_XATTR_ACCESS)==0) {
      r = cfuse->client->ll_acl_to_ugo(in,value,size,ctx->uid,ctx->gid);

      // -EINVAL 
      if(0 == r || -EINVAL == r)
            goto LAST ;
    }

    /*CIFS要求最多500条ACL by yuluxian*/
    if (strcmp(name, POSIX_ACL_XATTR_ACCESS)==0
        || strcmp(name, POSIX_ACL_XATTR_DEFAULT)==0) {
        
        if(size > ACL_CIFS_MAX_SIZE) {
          r = -ENOSPC;
          goto LAST;
        }
    }
    
    if(size > ACL_MAX_SIZE) {
      r = -ENOSPC;
      goto LAST;
    }
  }
  
  r = cfuse->client->ll_setxattr(in, name, value, size, flags, ctx->uid,
                                 ctx->gid);
LAST:
  fuse_reply_err(req, ERR_NUM(r));

  cfuse->iput(in); // iput required
}

static void fuse_ll_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(ino);
  char buf[size];
  int r =0;
  if(!DFTPERM) {
    r = cfuse->client->ll_permission_walk_parent(in, ctx->uid, ctx->gid,PERM_WALK_CHECK_EXEC);
    if(r) {
      fuse_reply_err(req, ERR_NUM(r));
      goto LAST;
    }
  }
  r = cfuse->client->ll_listxattr(in, buf, size, ctx->uid, ctx->gid);
  if (size == 0 && r >= 0)
    fuse_reply_xattr(req, r);
  else if (r >= 0)
    fuse_reply_buf(req, buf, r);
  else
    fuse_reply_err(req, -r);
LAST:
  cfuse->iput(in); // iput required
}

static void fuse_ll_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                             size_t size)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(ino);
  char buf[size];
  int r = 0;
  if(!DFTPERM) {
    r = cfuse->client->ll_permission_walk_parent(in, ctx->uid, ctx->gid,PERM_WALK_CHECK_EXEC);
    if(r) {
      fuse_reply_err(req, ERR_NUM(r));
      goto LAST;
    }
  }
  r = cfuse->client->ll_getxattr(in, name, buf, size, ctx->uid, ctx->gid);
  if (size == 0 && r >= 0)
    fuse_reply_xattr(req, r);
  else if (r >= 0)
    fuse_reply_buf(req, buf, r);
  else
    fuse_reply_err(req, -r);
LAST:
  cfuse->iput(in); // iput required
}

static void fuse_ll_removexattr(fuse_req_t req, fuse_ino_t ino,
                                const char *name)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(ino);
  struct stat stbuf;
  int r = 0;
  
  if(!DFTPERM) {
    /*r = cfuse->client->ll_permission_walk_parent(in, ctx->uid, ctx->gid, PERM_WALK_CHECK_EXEC);
    if(r)
    {
        fuse_reply_err(req, -r);
        goto LAST;
    }*/
    
    /* permission check*/
    r = cfuse->client->ll_stat(in, &stbuf);
    if (r)
    {
        goto LAST;
    }
    
    if ((ctx->uid != stbuf.st_uid) && (ctx->uid != 0))
    {
        r = -EPERM;
    
        goto LAST;
    }
  }
  
  r = cfuse->client->ll_removexattr(in, name, ctx->uid,
                                        ctx->gid);
LAST:
  fuse_reply_err(req, -r);
  cfuse->iput(in); // iput required
}

static void fuse_ll_opendir(fuse_req_t req, fuse_ino_t ino,
                            struct fuse_file_info *fi)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(ino);
  void *dirp;
  int r =0 ;
  if(!DFTPERM) {
    r = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid,PERM_WALK_CHECK_READ);
    if(r) {
      fuse_reply_err(req, ERR_NUM(r));
      goto LAST;
    }
  }

  r = cfuse->client->ll_opendir(in, (dir_result_t **) &dirp, ctx->uid,
                                ctx->gid);
  if (r >= 0) {
    fi->fh = (long)dirp;
    fuse_reply_open(req, fi);
  } else {
    fuse_reply_err(req, -r);
  }
LAST:
  cfuse->iput(in); // iput required
}

static void fuse_ll_readlink(fuse_req_t req, fuse_ino_t ino)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(ino);
  char buf[PATH_MAX + 1];  // leave room for a null terminator

  int r = cfuse->client->ll_readlink(in, buf, sizeof(buf) - 1, ctx->uid, ctx->gid);
  if (r >= 0) {
    buf[r] = '\0';
    fuse_reply_readlink(req, buf);
  } else {
    fuse_reply_err(req, -r);
  }

  cfuse->iput(in); // iput required
}

static void fuse_ll_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
                          mode_t mode, dev_t rdev)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *i2, *i1 = cfuse->iget(parent);
  struct fuse_entry_param fe;

  memset(&fe, 0, sizeof(fe));
  int r = 0;
  if(!DFTPERM) {
    r = cfuse->client->ll_permission_walk(i1, ctx->uid, ctx->gid,PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    if(r) {
      fuse_reply_err(req,ERR_NUM(r));
      goto LAST;
    }
  }

  //add by lvq
  if (cfuse->client->cct->_conf->fuse_posixacl) {
    if (!cfuse->client->is_parent_acl(i1, ctx->uid, ctx->gid))
    {
        mode &= ~(ctx->umask);
    }
  }
  
  r = cfuse->client->ll_mknod(i1, name, mode, new_decode_dev(rdev),
                              &fe.attr, &i2, ctx->uid, ctx->gid);
  if (r == 0) {
    fe.ino = cfuse->make_fake_ino(fe.attr.st_ino, fe.attr.st_dev);
    fe.attr.st_rdev = new_encode_dev(fe.attr.st_rdev);
    fuse_reply_entry(req, &fe);
  } else {
    fuse_reply_err(req, -r);
  }

  // XXX NB, we dont iput(i2) because FUSE will do so in a matching
  // fuse_ll_forget()
LAST:
  cfuse->iput(i1); // iput required
}

static void fuse_ll_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
                          mode_t mode)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *i2, *i1 = cfuse->iget(parent);
  struct fuse_entry_param fe;
  int r  = 0;
  memset(&fe, 0, sizeof(fe));
  
  if(!DFTPERM) {
    r = cfuse->client->ll_permission_walk(i1, ctx->uid, ctx->gid,PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    if(r) {
      fuse_reply_err(req,ERR_NUM(r));
      goto LAST;
    }
  }

  //add by lvq
  if (cfuse->client->cct->_conf->fuse_posixacl) {
    if (!cfuse->client->is_parent_acl(i1, ctx->uid, ctx->gid))
    {
        mode &= ~(ctx->umask);
    }
  }
  
  r = cfuse->client->ll_mkdir(i1, name, mode, &fe.attr, &i2, ctx->uid,
                              ctx->gid);
  if (r == 0) {

    if(!DFTPERM) {
      if(geteuid() == 0) {
        cfuse->client->ll_chown(i2, ctx->uid, ctx->gid);
      }
      cfuse->client->ll_fuse_init_acl(i2, /*S_IRWXU|S_IRWXG|S_IRWXO|S_IFDIR*/mode|S_IFDIR);
    }
	
    fe.ino = cfuse->make_fake_ino(fe.attr.st_ino, fe.attr.st_dev);
    fe.attr.st_rdev = new_encode_dev(fe.attr.st_rdev);
    fuse_reply_entry(req, &fe);
  } else {
    fuse_reply_err(req, -r);
  }

  // XXX NB, we dont iput(i2) because FUSE will do so in a matching
  // fuse_ll_forget()
LAST:
  cfuse->iput(i1); // iput required
}

static void fuse_ll_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(parent);
  struct fuse_entry_param fe;
  Inode *inChild = NULL; // see below
  int r = 0;
  
  if(!DFTPERM) {
    // 获取当前删除目录name的Inode
    memset(&fe, 0, sizeof(fe));
    r = cfuse->client->ll_lookup(in, name, &fe.attr, &inChild, ctx->uid, ctx->gid);
    if(r) {
      fuse_reply_err(req, ERR_NUM(r));
      goto LAST;
    }

    // 判断是否满足粘着位权限
    r = cfuse->client->ll_check_sticky_parent(in, inChild, ctx->uid);
    if(r) {
      fuse_reply_err(req, ERR_NUM(r));
      goto LAST;
    }

    /*modify by yuluxian ACL改造*/
    if (DFTPERM_ACL) {
        r = cfuse->client->ll_permission_walk(inChild, ctx->uid, ctx->gid, PERM_WALK_CHECK_DELETE);
    }
    else {
        r = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid,
						PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    }
    
    if(r) {
      fuse_reply_err(req,ERR_NUM(r));
      goto LAST;
    }
  }
  r = cfuse->client->ll_unlink(in, name, ctx->uid, ctx->gid);
  fuse_reply_err(req, -r);
LAST:

  cfuse->iput(in); // iput required
  if (inChild)
    cfuse->iput(inChild);
}

static void fuse_ll_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(parent);
  struct fuse_entry_param fe;
  Inode *inChild = NULL; // see below
  int r = 0;
  
  if(!DFTPERM) {
    // 获取当前删除目录name的Inode
    memset(&fe, 0, sizeof(fe));
    r = cfuse->client->ll_lookup(in, name, &fe.attr, &inChild, ctx->uid, ctx->gid);
    if(r) {
      fuse_reply_err(req, ERR_NUM(r));
      goto LAST;
    }

    // 判断是否满足粘着位权限
    r = cfuse->client->ll_check_sticky_parent(in, inChild, ctx->uid);
    if(r) {
      fuse_reply_err(req, ERR_NUM(r));
      goto LAST;
    }

    /*modify by yuluxian ACL改造*/
    if (DFTPERM_ACL) {
        r = cfuse->client->ll_permission_walk(inChild, ctx->uid, ctx->gid, PERM_WALK_CHECK_DELETE);
    }
    else {
        r = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid,
						PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    }
    if(r) {
      fuse_reply_err(req, ERR_NUM(r));
      goto LAST;
    }
  }
  r = cfuse->client->ll_rmdir(in, name, ctx->uid, ctx->gid);
  fuse_reply_err(req, -r);
LAST:
  cfuse->iput(in); // iput required
   if (inChild)
    cfuse->iput(inChild);
}

static void fuse_ll_symlink(fuse_req_t req, const char *existing,
                            fuse_ino_t parent, const char *name)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *i2, *i1 = cfuse->iget(parent);
  struct fuse_entry_param fe;

  memset(&fe, 0, sizeof(fe));
  int r = 0;
  if(!DFTPERM) {
    r = cfuse->client->ll_permission_walk(i1, ctx->uid, ctx->gid,PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    if(r) {
      fuse_reply_err(req, ERR_NUM(r));
      goto LAST;
    }
  }
  r = cfuse->client->ll_symlink(i1, name, existing, &fe.attr, &i2, ctx->uid,
                                ctx->gid);
  if (r == 0) {
    if(!DFTPERM) {
      if(geteuid() == 0) {
        cfuse->client->ll_chown(i2, ctx->uid, ctx->gid);
      }
    }
    fe.ino = cfuse->make_fake_ino(fe.attr.st_ino, fe.attr.st_dev);
    fe.attr.st_rdev = new_encode_dev(fe.attr.st_rdev);
    fuse_reply_entry(req, &fe);
  } else {
    fuse_reply_err(req, -r);
  }

  // XXX NB, we dont iput(i2) because FUSE will do so in a matching
  // fuse_ll_forget()
LAST:
  cfuse->iput(i1); // iput required
}

static void fuse_ll_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
                           fuse_ino_t newparent, const char *newname)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(parent);
  Inode *nin = cfuse->iget(newparent);
  int r = 0;
  // 目标目录/文件
  struct fuse_entry_param newfe;
  Inode *inNewChild = NULL; // see below
  
  // 查看原目录/文件
  struct fuse_entry_param fe;
  Inode *inChild = NULL; // see below

  if(!DFTPERM) {

    memset(&fe, 0, sizeof(fe));
    r = cfuse->client->ll_lookup(in, name, &fe.attr, &inChild, ctx->uid, ctx->gid);
    if(r) {
      fuse_reply_err(req, ERR_NUM(r));
      goto LAST;
    }

    /*DFTPERM_ACL未配置，则保持原逻辑，校验父目录w权限，如果配置，则校验当前目录删除权限*/
    if (!DFTPERM_ACL) {
        r = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid,
    										PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
        if(r) {
          fuse_reply_err(req,ERR_NUM(r));
          goto LAST;
        }
    } else if (inChild != NULL){
        r = cfuse->client->ll_permission_walk(inChild, ctx->uid, ctx->gid,PERM_WALK_CHECK_DELETE);
        if(r) {
          fuse_reply_err(req, ERR_NUM(r));
          goto LAST;
        }
    }
    
    r = cfuse->client->ll_permission_walk(nin, ctx->uid, ctx->gid,PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    if(r) {
      fuse_reply_err(req,ERR_NUM(r));
      goto LAST;
    }
    
    //////////////////check again

    //opi = in->get_son(string(name));
    if(!DFTPERM_ACL && inChild != NULL) {
      if(inChild->is_dir()) {
        /* see rename(2), need write permission for renamed directory, because
         * needed to update the .. entry
         */
        r = cfuse->client->ll_permission_walk(inChild, ctx->uid, ctx->gid,PERM_WALK_CHECK_WRITE);
        if(r) {
          fuse_reply_err(req, ERR_NUM(r));
          goto LAST;
        }
      }
    }    

    // 判断原目录/文件是否满足粘着位权限
    r = cfuse->client->ll_check_sticky_parent(in, inChild, ctx->uid);
    if(r)
    {
        fuse_reply_err(req, -r);
        goto LAST;
    }
    
    // 查看目标目录/文件
    memset(&newfe, 0, sizeof(newfe));
    r = cfuse->client->ll_lookup(nin, newname, &newfe.attr, &inNewChild, ctx->uid, ctx->gid);
    if(!r)
    {
        // 如果目标目录/文件存在需要校验粘着位
        // 判断目标目录/文件是否满足粘着位权限
        r = cfuse->client->ll_check_sticky_parent(nin, inNewChild, ctx->uid);
        if(r)
        {
            fuse_reply_err(req, -r);
            goto LAST;
        }
    }

    //////////////////check again
  }
  r = cfuse->client->ll_rename(in, name, nin, newname, ctx->uid, ctx->gid);
  fuse_reply_err(req, -r);
LAST:
  cfuse->iput(in); // iputs required
  cfuse->iput(nin);
  if (inChild)
  {
      cfuse->iput(inChild);
  }
  
  if (inNewChild)
  {
      cfuse->iput(inNewChild);
  }  
}

static void fuse_ll_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
                         const char *newname)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(ino);
  Inode *nin = cfuse->iget(newparent);
  struct fuse_entry_param fe;

  memset(&fe, 0, sizeof(fe));

  int r = 0;
  if(!DFTPERM) {
    r = cfuse->client->ll_permission_walk(nin, ctx->uid, ctx->gid,PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    if(r) {
      fuse_reply_err(req,ERR_NUM(r));
      goto LAST;
    }
    r = cfuse->client->ll_permission_walk_parent(in, ctx->uid, ctx->gid,PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    if(r) {
      fuse_reply_err(req,ERR_NUM(r));
      goto LAST;
    }

  }
  r = cfuse->client->ll_link(in, nin, newname, &fe.attr, ctx->uid,
                             ctx->gid);
  if (r == 0) {
    fe.ino = cfuse->make_fake_ino(fe.attr.st_ino, fe.attr.st_dev);
    fe.attr.st_rdev = new_encode_dev(fe.attr.st_rdev);
    fuse_reply_entry(req, &fe);
  } else {
    fuse_reply_err(req, -r);
  }
LAST:

  cfuse->iput(in); // iputs required
  cfuse->iput(nin);
}

static void fuse_ll_open(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info *fi)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(ino);
  Fh *fh = NULL;
  int r = 0;

  if(!DFTPERM) {

    r = cfuse->client->ll_permission_walk_parent(in, ctx->uid, ctx->gid,PERM_WALK_CHECK_EXEC);
    if(r) {
      fuse_reply_err(req,ERR_NUM(r));
      goto LAST;
    }

    if(fi->flags & O_WRONLY) {
      r = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid,PERM_WALK_CHECK_WRITE);
    } else if(fi->flags & O_RDWR) {
      r = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid,PERM_WALK_CHECK_READ|PERM_WALK_CHECK_WRITE);
    } else if ((fi->flags & O_TRUNC) || (fi->flags & O_APPEND)) {
      r = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid,PERM_WALK_CHECK_WRITE);
    } else {
      r = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid,PERM_WALK_CHECK_READ);
    }
    
    if(r) {
      fuse_reply_err(req, -r);
      goto LAST;
    }
  }
  r = cfuse->client->ll_open(in, fi->flags, &fh, ctx->uid, ctx->gid);
  if (r == 0) {
    fi->fh = (long)fh;
#if FUSE_VERSION >= FUSE_MAKE_VERSION(2, 8)
    if (cfuse->client->cct->_conf->fuse_use_invalidate_cb)
      fi->keep_cache = 1;
#endif
    fuse_reply_open(req, fi);
  } else {
    fuse_reply_err(req, -r);
  }
LAST:
  cfuse->iput(in); // iput required
}

static void fuse_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                         struct fuse_file_info *fi)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  Fh *fh = (Fh*)fi->fh;
  bufferlist bl;
  int r = cfuse->client->ll_read(fh, off, size, &bl);
  if (r >= 0)
    fuse_reply_buf(req, bl.c_str(), bl.length());
  else
    fuse_reply_err(req, -r);
}

static void fuse_ll_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
                          size_t size, off_t off, struct fuse_file_info *fi)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  Fh *fh = (Fh*)fi->fh;
  int r = cfuse->client->ll_write(fh, off, size, buf);
  if (r >= 0)
    fuse_reply_write(req, r);
  else
    fuse_reply_err(req, -r);
}

static void fuse_ll_flush(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info *fi)
{
  // NOOP
  fuse_reply_err(req, 0);
}

#ifdef FUSE_IOCTL_COMPAT
static void fuse_ll_ioctl(fuse_req_t req, fuse_ino_t ino, int cmd, void *arg, struct fuse_file_info *fi,
                          unsigned flags, const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);

  if (flags & FUSE_IOCTL_COMPAT) {
    fuse_reply_err(req, ENOSYS);
    return;
  }

  switch(cmd) {
  case CEPH_IOC_GET_LAYOUT: {
    struct ceph_file_layout layout;
    struct ceph_ioctl_layout l;
    Fh *fh = (Fh*)fi->fh;
    cfuse->client->ll_file_layout(fh->inode, &layout);
    l.stripe_unit = layout.fl_stripe_unit;
    l.stripe_count = layout.fl_stripe_count;
    l.object_size = layout.fl_object_size;
    l.data_pool = layout.fl_pg_pool;
    fuse_reply_ioctl(req, 0, &l, sizeof(struct ceph_ioctl_layout));
  }
  break;
  default:
    fuse_reply_err(req, EINVAL);
  }
}
#endif

#if FUSE_VERSION > FUSE_MAKE_VERSION(2, 9)

static void fuse_ll_fallocate(fuse_req_t req, fuse_ino_t ino, int mode,
                              off_t offset, off_t length,
                              struct fuse_file_info *fi)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  Fh *fh = (Fh*)fi->fh;
  int r = cfuse->client->ll_fallocate(fh, mode, offset, length);
  fuse_reply_err(req, -r);
}

#endif

static void fuse_ll_release(fuse_req_t req, fuse_ino_t ino,
                            struct fuse_file_info *fi)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  Fh *fh = (Fh*)fi->fh;
  int r = cfuse->client->ll_release(fh);
  fuse_reply_err(req, -r);
}

static void fuse_ll_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                          struct fuse_file_info *fi)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  Fh *fh = (Fh*)fi->fh;
  int r = cfuse->client->ll_fsync(fh, datasync);
  fuse_reply_err(req, -r);
}

struct readdir_context {
  fuse_req_t req;
  char *buf;
  size_t size;
  size_t pos; /* in buf */
  uint64_t snap;
};

/*
 * return 0 on success, -1 if out of space
 */
static int fuse_ll_add_dirent(void *p, struct dirent *de, struct stat *st,
                              int stmask, off_t next_off)
{
  struct readdir_context *c = (struct readdir_context *)p;
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(c->req);

  st->st_ino = cfuse->make_fake_ino(de->d_ino, c->snap);
  st->st_mode = DTTOIF(de->d_type);
  st->st_rdev = new_encode_dev(st->st_rdev);

  size_t room = c->size - c->pos;
  size_t entrysize = fuse_add_direntry(c->req, c->buf + c->pos, room,
                                       de->d_name, st, next_off);
  if (entrysize > room)
    return -ENOSPC;

  /* success */
  c->pos += entrysize;
  return 0;
}

/*
 * return 0 on success, -1 if out of space
 */
static int fuse_ll_add_dirent_acl(void *p, struct dirent *de, struct stat *st,
                              int stmask, off_t next_off)
{
  struct readdir_context *c = (struct readdir_context *)p;
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(c->req);
  const struct fuse_ctx *ctx = fuse_req_ctx(c->req);
  Inode *in = cfuse->iget(de->d_ino);
  
  if (in && in->is_dir()){
      int ret = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid, PERM_WALK_CHECK_EXEC);
	  if (ret){
		  cfuse->iput(in);
	      return 0;
	  }
  }
  if (in)
      cfuse->iput(in);
  
  st->st_ino = cfuse->make_fake_ino(de->d_ino, c->snap);
  st->st_mode = DTTOIF(de->d_type);
  st->st_rdev = new_encode_dev(st->st_rdev);

  size_t room = c->size - c->pos;
  size_t entrysize = fuse_add_direntry(c->req, c->buf + c->pos, room,
                                       de->d_name, st, next_off);
  if (entrysize > room)
    return -ENOSPC;

  /* success */
  c->pos += entrysize;
  return 0;
}

static void fuse_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                            off_t off, struct fuse_file_info *fi)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);

  dir_result_t *dirp = (dir_result_t*)fi->fh;
  cfuse->client->seekdir(dirp, off);

  struct readdir_context rc;
  rc.req = req;
  rc.buf = new char[size];
  rc.size = size;
  rc.pos = 0;
  rc.snap = cfuse->fino_snap(ino);

  int r;
  if (cfuse->client->cct->_conf->fuse_readdir_permission){
      r = cfuse->client->readdir_r_cb(dirp, fuse_ll_add_dirent_acl, &rc);
  }else {
      r = cfuse->client->readdir_r_cb(dirp, fuse_ll_add_dirent, &rc);
  }
  
  if (r == 0 || r == -ENOSPC)  /* ignore ENOSPC from our callback */
    fuse_reply_buf(req, rc.buf, rc.pos);
  else
    fuse_reply_err(req, -r);
  delete[] rc.buf;
}

static void fuse_ll_releasedir(fuse_req_t req, fuse_ino_t ino,
                               struct fuse_file_info *fi)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  dir_result_t *dirp = (dir_result_t*)fi->fh;
  cfuse->client->ll_releasedir(dirp);
  fuse_reply_err(req, 0);
}

static void fuse_ll_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
  //fuse_reply_err(req, 0);
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(ino);
  int res = 0;
  if(!DFTPERM) {
    if(mask & F_OK) {
      struct stat stbuf;
      res = cfuse->client->ll_stat(in, &stbuf);
      if(res) {
        fuse_reply_err(req, ERR_NUM(res));
        goto LAST;
      }
    }

    res = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid,
                                            (mask & R_OK)?PERM_WALK_CHECK_READ:0 |
                                            (mask & W_OK)?PERM_WALK_CHECK_WRITE:0 |
                                            (mask & X_OK)?PERM_WALK_CHECK_EXEC:0
                                           );
    if(res) {
      fuse_reply_err(req, ERR_NUM(res));
      goto LAST;
    }
  }
  fuse_reply_err(req, 0);
LAST:
  cfuse->iput(in);
}

static void fuse_ll_create(fuse_req_t req, fuse_ino_t parent, const char *name,
                           mode_t mode, struct fuse_file_info *fi)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *i1 = cfuse->iget(parent), *i2;
  struct fuse_entry_param fe;
  Fh *fh = NULL;
  int r = 0;
  memset(&fe, 0, sizeof(fe));

  if(!DFTPERM) {
    r = cfuse->client->ll_permission_walk(i1, ctx->uid, ctx->gid,PERM_WALK_CHECK_WRITE|PERM_WALK_CHECK_EXEC);
    if(r) {
      fuse_reply_err(req,ERR_NUM(r));
      goto LAST;
    }
  }

  //add by lvq
  if (cfuse->client->cct->_conf->fuse_posixacl) {
    if (!cfuse->client->is_parent_acl(i1, ctx->uid, ctx->gid))
    {
        mode &= ~(ctx->umask);
    }
  }
  
  // pass &i2 for the created inode so that ll_create takes an initial ll_ref
  r = cfuse->client->ll_create(i1, name, mode, fi->flags, &fe.attr, &i2,
                               &fh, ctx->uid, ctx->gid);
  if (r == 0) {
    if(!DFTPERM) {
      if(geteuid() == 0) {
        cfuse->client->ll_chown(i2,ctx->uid,ctx->gid);
      }
      cfuse->client->ll_fuse_init_acl(i2 , /*S_IRWXU|S_IRWXG|S_IRWXO*/mode);
    }
    fi->fh = (long)fh;
    fe.ino = cfuse->make_fake_ino(fe.attr.st_ino, fe.attr.st_dev);
	/*
    {
      char acl_xattr[XATTR_MAX_SIZE];
      if(cfuse->client->ll_getxattr(i1, POSIX_ACL_XATTR_DEFAULT, acl_xattr, XATTR_MAX_SIZE,getuid(),getgid())>0) {
		(const_cast<fuse_ctx *>(ctx))->umask = 0;
      }
    }*/
    fuse_reply_create(req, &fe, fi);
  } else
    fuse_reply_err(req, -r);
  // XXX NB, we dont iput(i2) because FUSE will do so in a matching
  // fuse_ll_forget()
LAST:
  cfuse->iput(i1); // iput required
}

static void fuse_ll_statfs(fuse_req_t req, fuse_ino_t ino)
{
  struct statvfs stbuf;
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  const struct fuse_ctx *ctx = fuse_req_ctx(req);
  Inode *in = cfuse->iget(ino);
  int r = 0;
  if(!DFTPERM) {
    r = cfuse->client->ll_permission_walk(in, ctx->uid, ctx->gid,0);
    if(r) {
      fuse_reply_err(req,ERR_NUM(r));
      goto LAST;
    }
  }
  r = cfuse->client->ll_statfs(in, &stbuf);
  if (r == 0)
    fuse_reply_statfs(req, &stbuf);
  else
    fuse_reply_err(req, -r);
LAST:
  cfuse->iput(in); // iput required
}

static void fuse_ll_getlk(fuse_req_t req, fuse_ino_t ino,
			  struct fuse_file_info *fi, struct flock *lock)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  Fh *fh = (Fh*)fi->fh;

  int r = cfuse->client->ll_getlk(fh, lock, fi->lock_owner);
  if (r == 0)
    fuse_reply_lock(req, lock);
  else
    fuse_reply_err(req, -r);
}

static void fuse_ll_setlk(fuse_req_t req, fuse_ino_t ino,
		          struct fuse_file_info *fi, struct flock *lock, int sleep)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  Fh *fh = (Fh*)fi->fh;

  // must use multithread if operation may block
  if (!cfuse->client->cct->_conf->fuse_multithreaded &&
      sleep && lock->l_type != F_UNLCK) {
    fuse_reply_err(req, EDEADLK);
    return;
  }

  int r = cfuse->client->ll_setlk(fh, lock, fi->lock_owner, sleep, req);
  fuse_reply_err(req, -r);
}

static void fuse_ll_interrupt(fuse_req_t req, void* data)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  cfuse->client->ll_interrupt(data);
}

static void switch_interrupt_cb(void *req, void* data)
{
  if (data)
    fuse_req_interrupt_func((fuse_req_t)req, fuse_ll_interrupt, data);
  else
    fuse_req_interrupt_func((fuse_req_t)req, NULL, NULL);
}

#if FUSE_VERSION >= FUSE_MAKE_VERSION(2, 9)
static void fuse_ll_flock(fuse_req_t req, fuse_ino_t ino,
		          struct fuse_file_info *fi, int cmd)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)fuse_req_userdata(req);
  Fh *fh = (Fh*)fi->fh;
  if (!cfuse->client->cct->_conf->fuse_multithreaded &&
      !(cmd & (LOCK_NB | LOCK_UN))) {
    fuse_reply_err(req, EDEADLK);
    return;
  }

  int r = cfuse->client->ll_flock(fh, cmd, fi->lock_owner, req);
  fuse_reply_err(req, -r);
}
#endif

#if 0
static int getgroups_cb(void *handle, uid_t uid, gid_t **sgids)
{
#if FUSE_VERSION >= FUSE_MAKE_VERSION(2, 8)
    CephFuse::Handle *cfuse = (CephFuse::Handle *)handle;
    fuse_req_t req = cfuse->get_fuse_req();

    assert(sgids);
    int c = fuse_req_getgroups(req, 0, NULL);
    if (c < 0)
        return c;

    if (c == 0)
        return 0;

    *sgids = (gid_t *)malloc(c * sizeof(**sgids));
    if (!*sgids)
        return -ENOMEM;

    c = fuse_req_getgroups(req, c, *sgids);
    if (c < 0)
    {
        free(*sgids);
        return c;
    }

    return c;
#else
    return -ENOSYS;
#endif
}
#endif
static void ino_invalidate_cb(void *handle, vinodeno_t vino, int64_t off,
                              int64_t len)
{
#if FUSE_VERSION >= FUSE_MAKE_VERSION(2, 8)
  CephFuse::Handle *cfuse = (CephFuse::Handle *)handle;
  fuse_ino_t fino = cfuse->make_fake_ino(vino.ino, vino.snapid);
  fuse_lowlevel_notify_inval_inode(cfuse->ch, fino, off, len);
#endif
}

static void dentry_invalidate_cb(void *handle, vinodeno_t dirino,
                                 vinodeno_t ino, string& name)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)handle;
  fuse_ino_t fdirino = cfuse->make_fake_ino(dirino.ino, dirino.snapid);
  int iret=0;
#if FUSE_VERSION >= FUSE_MAKE_VERSION(2, 9)
  fuse_ino_t fino = 0;
  if (ino.ino != inodeno_t())
    fino = cfuse->make_fake_ino(ino.ino, ino.snapid);
  iret = fuse_lowlevel_notify_delete(cfuse->ch, fdirino, fino, name.c_str(), name.length());
  if(iret < 0){
    fuse_lowlevel_notify_inval_entry(cfuse->ch, fdirino, name.c_str(), name.length());
  }
#elif FUSE_VERSION >= FUSE_MAKE_VERSION(2, 8)
  fuse_lowlevel_notify_inval_entry(cfuse->ch, fdirino, name.c_str(), name.length());
#endif
}

static void remount_cb(void *handle)
{
  // used for trimming kernel dcache. when remounting a file system, linux kernel
  // trims all unused dentries in the file system
  char cmd[1024];
  CephFuse::Handle *cfuse = (CephFuse::Handle *)handle;
  snprintf(cmd, sizeof(cmd), "mount -i -o remount %s", cfuse->mountpoint);
  system(cmd);
}

static void do_init(void *data, fuse_conn_info *bar)
{
  CephFuse::Handle *cfuse = (CephFuse::Handle *)data;
  if (cfuse->fd_on_success) {
    //cout << "fuse init signaling on fd " << fd_on_success << std::endl;
    uint32_t r = 0;
    int err = safe_write(cfuse->fd_on_success, &r, sizeof(r));
    if (err) {
      derr << "fuse_ll: do_init: safe_write failed with error "
           << cpp_strerror(err) << dendl;
      ceph_abort();
    }
    //cout << "fuse init done signaling on fd " << fd_on_success << std::endl;

    // close stdout, etc.
    ::close(0);
    ::close(1);
    ::close(2);
  }
}

const static struct fuse_lowlevel_ops fuse_ll_oper = {
init:
  do_init,
  destroy: 0,
lookup:
  fuse_ll_lookup,
forget:
  fuse_ll_forget,
getattr:
  fuse_ll_getattr,
setattr:
  fuse_ll_setattr,
readlink:
  fuse_ll_readlink,
mknod:
  fuse_ll_mknod,
mkdir:
  fuse_ll_mkdir,
unlink:
  fuse_ll_unlink,
rmdir:
  fuse_ll_rmdir,
symlink:
  fuse_ll_symlink,
rename:
  fuse_ll_rename,
link:
  fuse_ll_link,
open:
  fuse_ll_open,
read:
  fuse_ll_read,
write:
  fuse_ll_write,
flush:
  fuse_ll_flush,
release:
  fuse_ll_release,
fsync:
  fuse_ll_fsync,
opendir:
  fuse_ll_opendir,
readdir:
  fuse_ll_readdir,
releasedir:
  fuse_ll_releasedir,
  fsyncdir: 0,
statfs:
  fuse_ll_statfs,
setxattr:
  fuse_ll_setxattr,
getxattr:
  fuse_ll_getxattr,
listxattr:
  fuse_ll_listxattr,
removexattr:
  fuse_ll_removexattr,
access:
  fuse_ll_access,
create:
  fuse_ll_create,
  getlk: fuse_ll_getlk,
  setlk: fuse_ll_setlk,
  bmap: 0,
#if FUSE_VERSION >= FUSE_MAKE_VERSION(2, 8)
#ifdef FUSE_IOCTL_COMPAT
ioctl:
  fuse_ll_ioctl,
#else
  ioctl: 0,
#endif
  poll: 0,
#endif
#if FUSE_VERSION >= FUSE_MAKE_VERSION(2, 9)
  write_buf: 0,
  retrieve_reply: 0,
  forget_multi: 0,
  flock: fuse_ll_flock,
#endif
#if FUSE_VERSION > FUSE_MAKE_VERSION(2, 9)
fallocate:
  fuse_ll_fallocate
#endif
};


CephFuse::Handle::Handle(Client *c, int fd) :
  fd_on_success(fd),
  client(c),
  ch(NULL),
  se(NULL),
  mountpoint(NULL),
  stag_lock("fuse_ll.cc stag_lock"),
  last_stag(0)
{
  snap_stag_map[CEPH_NOSNAP] = 0;
  stag_snap_map[0] = CEPH_NOSNAP;
  memset(&args, 0, sizeof(args));
}

CephFuse::Handle::~Handle()
{
 // ceph_fuse_opt_free_args(&args);
  fuse_opt_free_args(&args);
}

void CephFuse::Handle::finalize()
{
  client->ll_register_callbacks(NULL);

  if (se)
    fuse_remove_signal_handlers(se);
  if (ch)
    fuse_session_remove_chan(ch);
  if (se)
    fuse_session_destroy(se);
  if (ch)
    fuse_unmount(mountpoint, ch);

}

int CephFuse::Handle::init(int argc, const char *argv[])
{
  // set up fuse argc/argv
  int newargc = 0;
  const char **newargv = (const char **) malloc((argc + 10) * sizeof(char *));
  if(!newargv)
    return ENOMEM;

  newargv[newargc++] = argv[0];
  newargv[newargc++] = "-f";  // stay in foreground

  //add by lvq
  if (client->cct->_conf->fuse_posixacl) {
    newargv[newargc++] = "-o";
    newargv[newargc++] = "posixacl";
  }
  
  if (client->cct->_conf->fuse_allow_other) {
    newargv[newargc++] = "-o";
    newargv[newargc++] = "allow_other";
  }
  dout(1)<<"-------------fuse_default_permissions: "<<client->cct->_conf->fuse_default_permissions<<dendl;
  if (client->cct->_conf->fuse_default_permissions) {
    newargv[newargc++] = "-o";
    newargv[newargc++] = "default_permissions";
  }
  if (client->cct->_conf->fuse_big_writes) {
    newargv[newargc++] = "-o";
    newargv[newargc++] = "big_writes";
  }
  if (client->cct->_conf->fuse_atomic_o_trunc) {
    newargv[newargc++] = "-o";
    newargv[newargc++] = "atomic_o_trunc";
  }

  if (client->cct->_conf->fuse_debug)
    newargv[newargc++] = "-d";

  for (int argctr = 1; argctr < argc; argctr++)
    newargv[newargc++] = argv[argctr];

  derr << "init, newargv = " << newargv << " newargc=" << newargc << dendl;
  struct fuse_args a = CEPH_FUSE_ARGS_INIT(newargc, (char**)newargv);
  args = a;  // Roundabout construction b/c FUSE_ARGS_INIT is for initialization not assignment

  if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) == -1) {
    derr << "fuse_parse_cmdline failed." << dendl;
    fuse_opt_free_args(&args);
    free(newargv);
    return EINVAL;
  }

  assert(args.allocated);  // Checking fuse has realloc'd args so we can free newargv
  free(newargv);
  return 0;
}


int CephFuse::Handle::start()
{
  if (client->cct->_conf->fuse_posixacl) {
    ch = ceph_fuse_mount(mountpoint, &args);
  }
  else {
    ch = fuse_mount(mountpoint, &args);
  }
  
  if (!ch) {
    derr << "fuse_mount(mountpoint=" << mountpoint << ") failed." << dendl;
    return EIO;
  }

  se = fuse_lowlevel_new(&args, &fuse_ll_oper, sizeof(fuse_ll_oper), this);
  if (!se) {
    derr << "fuse_lowlevel_new failed" << dendl;
    return EDOM;
  }

  signal(SIGTERM, SIG_DFL);
  signal(SIGINT, SIG_DFL);
  if (fuse_set_signal_handlers(se) == -1) {
    derr << "fuse_set_signal_handlers failed" << dendl;
    return ENOSYS;
  }

  fuse_session_add_chan(se, ch);
  client->ll_register_switch_interrupt_cb(switch_interrupt_cb);

  struct client_callback_args args = {
handle:
    this,
ino_cb:
    client->cct->_conf->fuse_use_invalidate_cb ? ino_invalidate_cb : NULL,
    dentry_cb: dentry_invalidate_cb,
    remount_cb: remount_cb
    /*
     * this is broken:
     *
     * - the cb needs the request handle to be useful; we should get the
     *   gids in the method here in fuse_ll.c and pass the gid list in,
     *   not use a callback.
     * - the callback mallocs the list but it is not free()'d
     *
     * so disable it for now...
     getgroups_cb: getgroups_cb,
     */
  };
  client->ll_register_callbacks(&args);

  return 0;
}

int CephFuse::Handle::loop()
{
  if (client->cct->_conf->fuse_multithreaded) {
    return fuse_session_loop_mt(se);
  } else {
    return fuse_session_loop(se);
  }
}

uint64_t CephFuse::Handle::fino_snap(uint64_t fino)
{
  Mutex::Locker l(stag_lock);
  uint64_t stag = FINO_STAG(fino);
  assert(stag_snap_map.count(stag));
  return stag_snap_map[stag];
}

vinodeno_t CephFuse::Handle::fino_vino(inodeno_t fino)
{
  if (fino.val == 1) {
    fino = inodeno_t(client->get_root_ino());
  }
  vinodeno_t vino(FINO_INO(fino), fino_snap(fino));
  //cout << "fino_vino " << fino << " -> " << vino << std::endl;
  return vino;
}

Inode * CephFuse::Handle::iget(inodeno_t fino)
{
  Inode *in =
    client->ll_get_inode(fino_vino(fino));
  return in;
}

void CephFuse::Handle::iput(Inode *in)
{
  client->ll_put(in);
}

uint64_t CephFuse::Handle::make_fake_ino(inodeno_t ino, snapid_t snapid)
{
  Mutex::Locker l(stag_lock);
  uint64_t stag;
  if (snap_stag_map.count(snapid) == 0) {
    stag = ++last_stag;
    snap_stag_map[snapid] = stag;
    stag_snap_map[stag] = snapid;
  } else
    stag = snap_stag_map[snapid];
  inodeno_t fino = MAKE_FINO(ino, stag);
  //cout << "make_fake_ino " << ino << "." << snapid << " -> " << fino << std::endl;
  return fino;
}

CephFuse::CephFuse(Client *c, int fd) : _handle(new CephFuse::Handle(c, fd))
{
}

CephFuse::~CephFuse()
{
  delete _handle;
}

int CephFuse::init(int argc, const char *argv[])
{
  return _handle->init(argc, argv);
}

int CephFuse::start()
{
  return _handle->start();
}

int CephFuse::loop()
{
  return _handle->loop();
}

void CephFuse::finalize()
{
  return _handle->finalize();
}

std::string CephFuse::get_mount_point() const
{
  if (_handle->mountpoint) {
    return _handle->mountpoint;
  } else {
    return "";
  }
}
