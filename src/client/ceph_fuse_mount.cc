
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#ifndef __NetBSD__
#include <mntent.h>
#endif
#include <pthread.h>
#include <utime.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <sys/uio.h>
#include <stdint.h>
#include <assert.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/param.h>

#define FUSE_USE_VERSION 30

#include <fuse/fuse.h>
#include <fuse/fuse_lowlevel.h>

#include "ceph_fuse_opt.h"

#define CEPH_FUSERMOUNT_PROG		"fusermount"
#define CEPH_FUSE_COMMFD_ENV		"_FUSE_COMMFD"

enum {
	KEY_KERN_FLAG,
	KEY_KERN_OPT,
	KEY_FUSERMOUNT_OPT,
	KEY_SUBTYPE_OPT,
	KEY_MTAB_OPT,
	KEY_ALLOW_ROOT,
	KEY_RO,
	KEY_HELP,
	KEY_VERSION,
};

struct ceph_mount_opts {
	int allow_other;
	int allow_root;
	int ishelp;
	int flags;
	int nonempty;
	int auto_unmount;
	int blkdev;
	char *fsname;
	char *subtype;
	char *subtype_opt;
	char *mtab_opts;
	char *fusermount_opts;
	char *kernel_opts;
};


#define CEPH_FUSE_MOUNT_OPT(t, p) { t, offsetof(struct ceph_mount_opts, p), 1 }

static const struct ceph_fuse_opt ceph_fuse_mount_opts[] = {
	CEPH_FUSE_MOUNT_OPT("allow_other",		allow_other),
	CEPH_FUSE_MOUNT_OPT("allow_root",		allow_root),
	CEPH_FUSE_MOUNT_OPT("nonempty",		nonempty),
	CEPH_FUSE_MOUNT_OPT("blkdev",		blkdev),
	CEPH_FUSE_MOUNT_OPT("auto_unmount",		auto_unmount),
	CEPH_FUSE_MOUNT_OPT("fsname=%s",		fsname),
	CEPH_FUSE_MOUNT_OPT("subtype=%s",		subtype),
	CEPH_FUSE_OPT_KEY("allow_other",		KEY_KERN_OPT),
	CEPH_FUSE_OPT_KEY("allow_root",		KEY_ALLOW_ROOT),
	CEPH_FUSE_OPT_KEY("nonempty",		KEY_FUSERMOUNT_OPT),
	CEPH_FUSE_OPT_KEY("auto_unmount",		KEY_FUSERMOUNT_OPT),
	CEPH_FUSE_OPT_KEY("blkdev",			KEY_FUSERMOUNT_OPT),
	CEPH_FUSE_OPT_KEY("fsname=",			KEY_FUSERMOUNT_OPT),
	CEPH_FUSE_OPT_KEY("subtype=",		KEY_SUBTYPE_OPT),
	CEPH_FUSE_OPT_KEY("large_read",		KEY_KERN_OPT),
	CEPH_FUSE_OPT_KEY("blksize=",		KEY_KERN_OPT),
	CEPH_FUSE_OPT_KEY("default_permissions",	KEY_KERN_OPT),
	CEPH_FUSE_OPT_KEY("max_read=",		KEY_KERN_OPT),
	CEPH_FUSE_OPT_KEY("max_read=",		CEPH_FUSE_OPT_KEY_KEEP),
	CEPH_FUSE_OPT_KEY("user=",			KEY_MTAB_OPT),
	CEPH_FUSE_OPT_KEY("-r",			KEY_RO),
	CEPH_FUSE_OPT_KEY("ro",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("rw",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("suid",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("nosuid",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("dev",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("nodev",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("exec",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("noexec",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("async",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("sync",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("dirsync",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("atime",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("noatime",			KEY_KERN_FLAG),
	CEPH_FUSE_OPT_KEY("posixacl",			KEY_KERN_FLAG), //add by lvq
	CEPH_FUSE_OPT_KEY("-h",			KEY_HELP),
	CEPH_FUSE_OPT_KEY("--help",			KEY_HELP),
	CEPH_FUSE_OPT_KEY("-V",			KEY_VERSION),
	CEPH_FUSE_OPT_KEY("--version",		KEY_VERSION),
	CEPH_FUSE_OPT_END
};

struct ceph_mount_flags {
	const char *opt;
	unsigned long flag;
	int on;
};

static const struct ceph_mount_flags ceph_mount_flags[] = {
	{"rw",	    MS_RDONLY,	    0},
	{"ro",	    MS_RDONLY,	    1},
	{"suid",    MS_NOSUID,	    0},
	{"nosuid",  MS_NOSUID,	    1},
	{"dev",	    MS_NODEV,	    0},
	{"nodev",   MS_NODEV,	    1},
	{"exec",    MS_NOEXEC,	    0},
	{"noexec",  MS_NOEXEC,	    1},
	{"async",   MS_SYNCHRONOUS, 0},
	{"sync",    MS_SYNCHRONOUS, 1},
	{"atime",   MS_NOATIME,	    0},
	{"noatime", MS_NOATIME,	    1},
	{"posixacl", MS_POSIXACL,	    1},  //add by lvq
#ifndef __NetBSD__
	{"dirsync", MS_DIRSYNC,	    1},
#endif
	{NULL,	    0,		    0}
};

static int ceph_mtab_needs_update(const char *mnt)
{
	int res;
	struct stat stbuf;

	/* If mtab is within new mount, don't touch it */
	if (strncmp(mnt, _PATH_MOUNTED, strlen(mnt)) == 0 &&
	    _PATH_MOUNTED[strlen(mnt)] == '/')
		return 0;

	/*
	 * Skip mtab update if /etc/mtab:
	 *
	 *  - doesn't exist,
	 *  - is a symlink,
	 *  - is on a read-only filesystem.
	 */
	res = lstat(_PATH_MOUNTED, &stbuf);
	if (res == -1) {
		if (errno == ENOENT)
			return 0;
	} else {
		uid_t ruid;
		int err;

		if (S_ISLNK(stbuf.st_mode))
			return 0;

		ruid = getuid();
		if (ruid != 0)
			setreuid(0, -1);

		res = access(_PATH_MOUNTED, W_OK);
		err = (res == -1) ? errno : 0;
		if (ruid != 0)
			setreuid(ruid, -1);

		if (err == EROFS)
			return 0;
	}

	return 1;
}

static int ceph_add_mount(const char *progname, const char *fsname,
		       const char *mnt, const char *type, const char *opts)
{
	int res;
	int status;
	sigset_t blockmask;
	sigset_t oldmask;

	sigemptyset(&blockmask);
	sigaddset(&blockmask, SIGCHLD);
	res = sigprocmask(SIG_BLOCK, &blockmask, &oldmask);
	if (res == -1) {
		fprintf(stderr, "%s: sigprocmask: %s\n", progname, strerror(errno));
		return -1;
	}

	res = fork();
	if (res == -1) {
		fprintf(stderr, "%s: fork: %s\n", progname, strerror(errno));
		goto out_restore;
	}
	if (res == 0) {
		sigprocmask(SIG_SETMASK, &oldmask, NULL);
		setuid(geteuid());
		execl("/bin/mount", "/bin/mount", "--no-canonicalize", "-i",
		      "-f", "-t", type, "-o", opts, fsname, mnt, NULL);
		fprintf(stderr, "%s: failed to execute /bin/mount: %s\n",
			progname, strerror(errno));
		exit(1);
	}
	res = waitpid(res, &status, 0);
	if (res == -1)
		fprintf(stderr, "%s: waitpid: %s\n", progname, strerror(errno));

	if (status != 0)
		res = -1;

 out_restore:
	sigprocmask(SIG_SETMASK, &oldmask, NULL);

	return res;
}

static int ceph_fuse_mnt_add_mount(const char *progname, const char *fsname,
		       const char *mnt, const char *type, const char *opts)
{
	if (!ceph_mtab_needs_update(mnt))
		return 0;

	return ceph_add_mount(progname, fsname, mnt, type, opts);
}

static char *ceph_fuse_mnt_resolve_path(const char *progname, const char *orig)
{
	char buf[PATH_MAX];
	char *copy;
	char *dst;
	char *end;
	char *lastcomp;
	const char *toresolv;

	if (!orig[0]) {
		fprintf(stderr, "%s: invalid mountpoint '%s'\n", progname,
			orig);
		return NULL;
	}

	copy = strdup(orig);
	if (copy == NULL) {
		fprintf(stderr, "%s: failed to allocate memory\n", progname);
		return NULL;
	}

	toresolv = copy;
	lastcomp = NULL;
	for (end = copy + strlen(copy) - 1; end > copy && *end == '/'; end --);
	if (end[0] != '/') {
		char *tmp;
		end[1] = '\0';
		tmp = strrchr(copy, '/');
		if (tmp == NULL) {
			lastcomp = copy;
			toresolv = ".";
		} else {
			lastcomp = tmp + 1;
			if (tmp == copy)
				toresolv = "/";
		}
		if (strcmp(lastcomp, ".") == 0 || strcmp(lastcomp, "..") == 0) {
			lastcomp = NULL;
			toresolv = copy;
		}
		else if (tmp)
			tmp[0] = '\0';
	}
	if (realpath(toresolv, buf) == NULL) {
		fprintf(stderr, "%s: bad mount point %s: %s\n", progname, orig,
			strerror(errno));
		free(copy);
		return NULL;
	}
	if (lastcomp == NULL)
		dst = strdup(buf);
	else {
		dst = (char *) malloc(strlen(buf) + 1 + strlen(lastcomp) + 1);
		if (dst) {
			unsigned buflen = strlen(buf);
			if (buflen && buf[buflen-1] == '/')
				sprintf(dst, "%s%s", buf, lastcomp);
			else
				sprintf(dst, "%s/%s", buf, lastcomp);
		}
	}
	free(copy);
	if (dst == NULL)
		fprintf(stderr, "%s: failed to allocate memory\n", progname);
	return dst;
}

static int ceph_fuse_mnt_check_empty(const char *progname, const char *mnt,
			 mode_t rootmode, off_t rootsize)
{
	int isempty = 1;

	if (S_ISDIR(rootmode)) {
		struct dirent *ent;
		DIR *dp = opendir(mnt);
		if (dp == NULL) {
			fprintf(stderr,
				"%s: failed to open mountpoint for reading: %s\n",
				progname, strerror(errno));
			return -1;
		}
		while ((ent = readdir(dp)) != NULL) {
			if (strcmp(ent->d_name, ".") != 0 &&
			    strcmp(ent->d_name, "..") != 0) {
				isempty = 0;
				break;
			}
		}
		closedir(dp);
	} else if (rootsize)
		isempty = 0;

	if (!isempty) {
		fprintf(stderr, "%s: mountpoint is not empty\n", progname);
		fprintf(stderr, "%s: if you are sure this is safe, use the 'nonempty' mount option\n", progname);
		return -1;
	}
	return 0;
}

static int ceph_fuse_mnt_check_fuseblk(void)
{
	char buf[256];
	FILE *f = fopen("/proc/filesystems", "r");
	if (!f)
		return 1;

	while (fgets(buf, sizeof(buf), f))
		if (strstr(buf, "fuseblk\n")) {
			fclose(f);
			return 1;
		}

	fclose(f);
	return 0;
}

static void ceph_set_mount_flag(const char *s, int *flags)
{
	int i;

	for (i = 0; ceph_mount_flags[i].opt != NULL; i++) {
		const char *opt = ceph_mount_flags[i].opt;
		if (strcmp(opt, s) == 0) {
			if (ceph_mount_flags[i].on)
				*flags |= ceph_mount_flags[i].flag;
			else
				*flags &= ~ceph_mount_flags[i].flag;
			return;
		}
	}
	fprintf(stderr, "fuse: internal error, can't find mount flag\n");
	abort();
}


static int ceph_get_mnt_flag_opts(char **mnt_optsp, int flags)
{
	int i;

	if (!(flags & MS_RDONLY) && ceph_fuse_opt_add_opt(mnt_optsp, "rw") == -1)
		return -1;

	for (i = 0; ceph_mount_flags[i].opt != NULL; i++) {
		if (ceph_mount_flags[i].on && (flags & ceph_mount_flags[i].flag) &&
		    ceph_fuse_opt_add_opt(mnt_optsp, ceph_mount_flags[i].opt) == -1)
			return -1;
	}
	return 0;
}

static int ceph_fuse_mount_sys(const char *mnt, struct ceph_mount_opts *mo,
			  const char *mnt_opts)
{
	char tmp[128];
	const char *devname = "/dev/fuse";
	char *source = NULL;
	char *type = NULL;
	struct stat stbuf;
	int fd;
	int res;

	if (!mnt) {
		fprintf(stderr, "fuse: missing mountpoint parameter\n");
		return -1;
	}

	res = stat(mnt, &stbuf);
	if (res == -1) {
		fprintf(stderr ,"fuse: failed to access mountpoint %s: %s\n",
			mnt, strerror(errno));
		return -1;
	}

	if (!mo->nonempty) {
		res = ceph_fuse_mnt_check_empty("fuse", mnt, stbuf.st_mode,
					   stbuf.st_size);
		if (res == -1)
			return -1;
	}

	if (mo->auto_unmount) {
		/* Tell the caller to fallback to fusermount because
		   auto-unmount does not work otherwise. */
		return -2;
	}

	fd = open(devname, O_RDWR);
	if (fd == -1) {
		if (errno == ENODEV || errno == ENOENT)
			fprintf(stderr, "fuse: device not found, try 'modprobe fuse' first\n");
		else
			fprintf(stderr, "fuse: failed to open %s: %s\n",
				devname, strerror(errno));
		return -1;
	}

	snprintf(tmp, sizeof(tmp),  "fd=%i,rootmode=%o,user_id=%i,group_id=%i",
		 fd, stbuf.st_mode & S_IFMT, getuid(), getgid());

	res = ceph_fuse_opt_add_opt(&mo->kernel_opts, tmp);
	if (res == -1)
		goto out_close;

	source = (char *)malloc((mo->fsname ? strlen(mo->fsname) : 0) 
            + (mo->subtype ? strlen(mo->subtype) : 0) 
            + strlen(devname) + 32);

	type = (char *)malloc((mo->subtype ? strlen(mo->subtype) : 0) + 32);
	if (!type || !source) {
		fprintf(stderr, "fuse: failed to allocate memory\n");
		goto out_close;
	}

	strcpy(type, mo->blkdev ? "fuseblk" : "fuse");
	if (mo->subtype) {
		strcat(type, ".");
		strcat(type, mo->subtype);
	}
	strcpy(source,
	       mo->fsname ? mo->fsname : (mo->subtype ? mo->subtype : devname));

	res = mount(source, mnt, type, mo->flags, mo->kernel_opts);
	if (res == -1 && errno == ENODEV && mo->subtype) {
		/* Probably missing subtype support */
		strcpy(type, mo->blkdev ? "fuseblk" : "fuse");
		if (mo->fsname) {
			if (!mo->blkdev)
				sprintf(source, "%s#%s", mo->subtype,
					mo->fsname);
		} else {
			strcpy(source, type);
		}
		res = mount(source, mnt, type, mo->flags, mo->kernel_opts);
	}
	if (res == -1) {
		/*
		 * Maybe kernel doesn't support unprivileged mounts, in this
		 * case try falling back to fusermount
		 */
		if (errno == EPERM) {
			res = -2;
		} else {
			int errno_save = errno;
			if (mo->blkdev && errno == ENODEV &&
			    !ceph_fuse_mnt_check_fuseblk())
				fprintf(stderr,
					"fuse: 'fuseblk' support missing\n");
			else
				fprintf(stderr, "fuse: mount failed: %s\n",
					strerror(errno_save));
		}

		goto out_close;
	}

#ifndef __NetBSD__
#ifndef IGNORE_MTAB
	if (geteuid() == 0) {
		char *newmnt = ceph_fuse_mnt_resolve_path("fuse", mnt);
		res = -1;
		if (!newmnt)
			goto out_umount;

		res = ceph_fuse_mnt_add_mount("fuse", source, newmnt, type,
					 mnt_opts);
		free(newmnt);
		if (res == -1)
			goto out_umount;
	}
#endif /* IGNORE_MTAB */
#endif /* __NetBSD__ */
	free(type);
	free(source);

	return fd;

out_umount:
	umount2(mnt, 2); /* lazy umount */
out_close:
	free(type);
	free(source);
	close(fd);
	return res;
}

/* return value:
 * >= 0	 => fd
 * -1	 => error
 */
static int ceph_receive_fd(int fd)
{
	struct msghdr msg;
	struct iovec iov;
	char buf[1];
	int rv;
	size_t ccmsg[CMSG_SPACE(sizeof(int)) / sizeof(size_t)];
	struct cmsghdr *cmsg;

	iov.iov_base = buf;
	iov.iov_len = 1;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	/* old BSD implementations should use msg_accrights instead of
	 * msg_control; the interface is different. */
	msg.msg_control = ccmsg;
	msg.msg_controllen = sizeof(ccmsg);

	while(((rv = recvmsg(fd, &msg, 0)) == -1) && errno == EINTR);
	if (rv == -1) {
		perror("recvmsg");
		return -1;
	}
	if(!rv) {
		/* EOF */
		return -1;
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg->cmsg_type == SCM_RIGHTS) {
		fprintf(stderr, "got control message of unknown type %d\n",
			cmsg->cmsg_type);
		return -1;
	}
	return *(int*)CMSG_DATA(cmsg);
}

static void ceph_exec_fusermount(const char *argv[])
{
    char buf[PATH_MAX];
    char fusermount_path[PATH_MAX];
    FILE *fp = popen("whereis -b fusermount", "r");

    memset(buf, 0x00, sizeof(buf));
    while (NULL !=fgets(buf, 1024, fp) ) {
            break;
    }

    char *pStr = strchr(buf, '/');
    memset(fusermount_path, 0x00, sizeof(fusermount_path));
    if (pStr) {
        int len = strlen(buf);
        if ('\n' == buf[len - 1])
            buf[len - 1] = '\0';
        
        strcpy(fusermount_path, pStr);
    }
    
    //execv(FUSERMOUNT_DIR "/" CEPH_FUSERMOUNT_PROG, (char **) argv);
    execv(fusermount_path, (char **) argv);
    execvp(CEPH_FUSERMOUNT_PROG, (char **) argv);
}

static int ceph_fuse_mount_fusermount(const char *mountpoint, struct ceph_mount_opts *mo,
		const char *opts, int quiet)
{
	int fds[2], pid;
	int res;
	int rv;

	if (!mountpoint) {
		fprintf(stderr, "fuse: missing mountpoint parameter\n");
		return -1;
	}

	res = socketpair(PF_UNIX, SOCK_STREAM, 0, fds);
	if(res == -1) {
		perror("fuse: socketpair() failed");
		return -1;
	}

	pid = fork();
	if(pid == -1) {
		perror("fuse: fork() failed");
		close(fds[0]);
		close(fds[1]);
		return -1;
	}

	if(pid == 0) {
		char env[10];
		const char *argv[32];
		int a = 0;

		if (quiet) {
			int fd = open("/dev/null", O_RDONLY);
			if (fd != -1) {
				dup2(fd, 1);
				dup2(fd, 2);
			}
		}

		argv[a++] = CEPH_FUSERMOUNT_PROG;
		if (opts) {
			argv[a++] = "-o";
			argv[a++] = opts;
		}
		argv[a++] = "--";
		argv[a++] = mountpoint;
		argv[a++] = NULL;

		close(fds[1]);
		fcntl(fds[0], F_SETFD, 0);
		snprintf(env, sizeof(env), "%i", fds[0]);
		setenv(CEPH_FUSE_COMMFD_ENV, env, 1);
		ceph_exec_fusermount(argv);
		perror("fuse: failed to exec fusermount");
		_exit(1);
	}

	close(fds[0]);
	rv = ceph_receive_fd(fds[1]);

	if (!mo->auto_unmount) {
		/* with auto_unmount option fusermount will not exit until 
		   this socket is closed */
		close(fds[1]);
		waitpid(pid, NULL, 0); /* bury zombie */
	}

	return rv;
}

static void ceph_mount_help(void)
{
	fprintf(stderr,
"    -o allow_other         allow access to other users\n"
"    -o allow_root          allow access to root\n"
"    -o auto_unmount        auto unmount on process termination\n"
"    -o nonempty            allow mounts over non-empty file/dir\n"
"    -o default_permissions enable permission checking by kernel\n"
"    -o fsname=NAME         set filesystem name\n"
"    -o subtype=NAME        set filesystem type\n"
"    -o large_read          issue large read requests (2.4 only)\n"
"    -o max_read=N          set maximum size of read requests\n"
"\n");
}

static void ceph_mount_version(void)
{
	int pid = fork();
	if (!pid) {
		const char *argv[] = { CEPH_FUSERMOUNT_PROG, "--version", NULL };
		ceph_exec_fusermount(argv);
		_exit(1);
	} else if (pid != -1)
		waitpid(pid, NULL, 0);
}


static int ceph_fuse_mount_opt_proc(void *data, const char *arg, int key,
			       struct fuse_args *outargs)
{
	struct ceph_mount_opts *mo = (struct ceph_mount_opts *)data;

	switch (key) {
	case KEY_ALLOW_ROOT:
		if (ceph_fuse_opt_add_opt(&mo->kernel_opts, "allow_other") == -1 ||
		    ceph_fuse_opt_add_arg(outargs, "-oallow_root") == -1)
			return -1;
		return 0;

	case KEY_RO:
		arg = "ro";
		/* fall through */
	case KEY_KERN_FLAG:
		ceph_set_mount_flag(arg, &mo->flags);
		return 0;

	case KEY_KERN_OPT:
		return ceph_fuse_opt_add_opt(&mo->kernel_opts, arg);

	case KEY_FUSERMOUNT_OPT:
		return ceph_fuse_opt_add_opt_escaped(&mo->fusermount_opts, arg);

	case KEY_SUBTYPE_OPT:
		return ceph_fuse_opt_add_opt(&mo->subtype_opt, arg);

	case KEY_MTAB_OPT:
		return ceph_fuse_opt_add_opt(&mo->mtab_opts, arg);

	case KEY_HELP:
		ceph_mount_help();
		mo->ishelp = 1;
		break;

	case KEY_VERSION:
		ceph_mount_version();
		mo->ishelp = 1;
		break;
	}
	return 1;
}


int ceph_fuse_kern_mount(const char *mountpoint, struct fuse_args *args)
{
       long addr = 0;
	struct ceph_mount_opts mo;
	int res = -1;
	char *mnt_opts = NULL;

	memset(&mo, 0, sizeof(mo));
	mo.flags = MS_NOSUID | MS_NODEV;

	if (args &&
	    ceph_fuse_opt_parse(args, &mo, ceph_fuse_mount_opts, ceph_fuse_mount_opt_proc) == -1)
		return -1;

	if (mo.allow_other && mo.allow_root) {
		fprintf(stderr, "fuse: 'allow_other' and 'allow_root' options are mutually exclusive\n");
		goto out;
	}
	res = 0;
	if (mo.ishelp)
		goto out;

	res = -1;
	if (ceph_get_mnt_flag_opts(&mnt_opts, mo.flags) == -1)
		goto out;
	if (mo.kernel_opts && ceph_fuse_opt_add_opt(&mnt_opts, mo.kernel_opts) == -1)
		goto out;
	if (mo.mtab_opts &&  ceph_fuse_opt_add_opt(&mnt_opts, mo.mtab_opts) == -1)
		goto out;

	res = ceph_fuse_mount_sys(mountpoint, &mo, mnt_opts);
	if (res == -2) {
		if (mo.fusermount_opts &&
		    ceph_fuse_opt_add_opt(&mnt_opts, mo.fusermount_opts) == -1)
			goto out;

		if (mo.subtype) {
			char *tmp_opts = NULL;

			res = -1;
			if (ceph_fuse_opt_add_opt(&tmp_opts, mnt_opts) == -1 ||
			    ceph_fuse_opt_add_opt(&tmp_opts, mo.subtype_opt) == -1) {
				free(tmp_opts);
				goto out;
			}

			res = ceph_fuse_mount_fusermount(mountpoint, &mo, tmp_opts, 1);
			free(tmp_opts);
			if (res == -1)
				res = ceph_fuse_mount_fusermount(mountpoint, &mo,
							    mnt_opts, 0);
		} else {
			res = ceph_fuse_mount_fusermount(mountpoint, &mo, mnt_opts, 0);
		}
	}
out:
	free(mnt_opts);
	free(mo.fsname);
	free(mo.subtype);
	free(mo.fusermount_opts);
	free(mo.subtype_opt);
	free(mo.kernel_opts);
	free(mo.mtab_opts);
	return res;
}

int ceph_fuse_mount_compat25(const char *mountpoint, struct fuse_args *args)
{
	return ceph_fuse_kern_mount(mountpoint, args);
}

static int ceph_exec_umount(const char *progname, const char *rel_mnt, int lazy)
{
	int res;
	int status;
	sigset_t blockmask;
	sigset_t oldmask;

	sigemptyset(&blockmask);
	sigaddset(&blockmask, SIGCHLD);
	res = sigprocmask(SIG_BLOCK, &blockmask, &oldmask);
	if (res == -1) {
		fprintf(stderr, "%s: sigprocmask: %s\n", progname, strerror(errno));
		return -1;
	}

	res = fork();
	if (res == -1) {
		fprintf(stderr, "%s: fork: %s\n", progname, strerror(errno));
		goto out_restore;
	}
	if (res == 0) {
		sigprocmask(SIG_SETMASK, &oldmask, NULL);
		setuid(geteuid());
		execl("/bin/umount", "/bin/umount", "-i", rel_mnt,
		      lazy ? "-l" : NULL, NULL);
		fprintf(stderr, "%s: failed to execute /bin/umount: %s\n",
			progname, strerror(errno));
		exit(1);
	}
	res = waitpid(res, &status, 0);
	if (res == -1)
		fprintf(stderr, "%s: waitpid: %s\n", progname, strerror(errno));

	if (status != 0) {
		res = -1;
	}

 out_restore:
	sigprocmask(SIG_SETMASK, &oldmask, NULL);
	return res;

}

int ceph_fuse_mnt_umount(const char *progname, const char *abs_mnt,
		    const char *rel_mnt, int lazy)
{
	int res;

	if (!ceph_mtab_needs_update(abs_mnt)) {
		res = umount2(rel_mnt, lazy ? 2 : 0);
		if (res == -1)
			fprintf(stderr, "%s: failed to unmount %s: %s\n",
				progname, abs_mnt, strerror(errno));
		return res;
	}

	return ceph_exec_umount(progname, rel_mnt, lazy);
}

static void ceph_fuse_kern_unmount(const char *mountpoint, int fd)
{
	int res;
	int pid;

	if (!mountpoint)
		return;

	if (fd != -1) {
		struct pollfd pfd;

		pfd.fd = fd;
		pfd.events = 0;
		res = poll(&pfd, 1, 0);

		/* Need to close file descriptor, otherwise synchronous umount
		   would recurse into filesystem, and deadlock.

		   Caller expects ceph_fuse_kern_unmount to close the fd, so close it
		   anyway. */
		close(fd);

		/* If file poll returns POLLERR on the device file descriptor,
		   then the filesystem is already unmounted */
		if (res == 1 && (pfd.revents & POLLERR))
			return;
	}

	if (geteuid() == 0) {
		ceph_fuse_mnt_umount("fuse", mountpoint, mountpoint,  1);
		return;
	}

	res = umount2(mountpoint, 2);
	if (res == 0)
		return;

	pid = fork();
	if(pid == -1)
		return;

	if(pid == 0) {
		const char *argv[] = { CEPH_FUSERMOUNT_PROG, "-u", "-q", "-z",
				       "--", mountpoint, NULL };

		ceph_exec_fusermount(argv);
		_exit(1);
	}
	waitpid(pid, NULL, 0);
}

struct fuse_lib_func
{
    struct fuse_chan * (*fuse_kern_chan_new)(int fd);
    void (*fuse_kern_unmount)(const char *mountpoint, int fd);
    int (*fuse_mount_sys)(const char *, struct mount_opts *,const char *);
};

static struct fuse_chan *ceph_fuse_mount_common(const char *mountpoint,
					   struct fuse_args *args)
{
	struct fuse_chan *ch;
	int fd;
       long addr =0;
       long addr1 =0;

       void *handle = dlopen("libfuse.so", RTLD_NOW|RTLD_GLOBAL);
       if (!handle) {
           return NULL;
       }

       struct fuse_lib_func fuse_lib_func;
       fuse_lib_func.fuse_kern_chan_new = (struct fuse_chan * (*)(int fd))dlsym(handle , "fuse_kern_chan_new");
       //fuse_lib_func.fuse_kern_unmount = (void (*)(const char *mountpoint, int fd))dlsym(handle , "fuse_kern_unmount");
       printf("fuse_lib_func.fuse_kern_chan_new = %p\n", fuse_lib_func.fuse_kern_chan_new);
       //printf("fuse_lib_func.fuse_kern_unmount = %p\n", fuse_lib_func.fuse_kern_unmount);
        
	/*
	 * Make sure file descriptors 0, 1 and 2 are open, otherwise chaos
	 * would ensue.
	 */
	do {
		fd = open("/dev/null", O_RDWR);
		if (fd > 2)
			close(fd);
	} while (fd >= 0 && fd <= 2);

	fd = ceph_fuse_mount_compat25(mountpoint, args);
	if (fd == -1)
		return NULL;

	ch = fuse_lib_func.fuse_kern_chan_new(fd);
	if (!ch)
             ceph_fuse_kern_unmount(mountpoint, fd);

       dlclose(handle);
	return ch;
}

struct fuse_chan *ceph_fuse_mount(const char *mountpoint, struct fuse_args *args)
{
	return ceph_fuse_mount_common(mountpoint, args);
}

