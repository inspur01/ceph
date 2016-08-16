#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "proc_flock.h"

#define FILENAME "/etc/icfs/.lock_file"

struct proc_flock_t {
    int fd;
};

struct proc_flock_t proc_flock;

static int proc_flock_init (void) {
	memset(&proc_flock, 0, sizeof(struct proc_flock_t));
	proc_flock.fd = open(FILENAME, O_RDONLY|O_CREAT|O_TRUNC);
	return proc_flock.fd;
}

static int proc_flock_lock (void) {
	if (proc_flock.fd <= 0)
		return 1;
	if (-1 == flock(proc_flock.fd, LOCK_EX)) {//阻塞等待
		return 0;
	} else {
		return 1;
	}
}

static int proc_flock_unlock (void) {
	if (proc_flock.fd <= 0)
		return 1;
	if (-1 == flock(proc_flock.fd, LOCK_UN)) {
		return 0;
	} else {
		return 1;
	}
}

static int proc_flock_destroy (void) {
	if (proc_flock.fd <= 0)
		return 1;
	close(proc_flock.fd);
	return 1;
}

int DB_BEGIN(void) {
	return proc_flock_init();
}

int DB_LOCK(void) {
	return proc_flock_lock();
}

int DB_UNLOCK(void) {
	return proc_flock_unlock();
}

int DB_END(void) {
	return proc_flock_destroy();
}





