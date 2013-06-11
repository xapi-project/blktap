/*
 * Copyright (C) Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.1 only
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef _LARGEFILE_SOURCE
#undef _LARGEFILE_SOURCE
#endif

#ifdef _LARGEFILE64_SOURCE
#undef _LARGEFILE64_SOURCE
#endif

#if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS == 64
#undef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 32
#endif

#ifdef _LARGEFILE_SOURCE
#undef _LARGEFILE_SOURCE
#endif

#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <signal.h>
#include <malloc.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <linux/hdreg.h>

#define _FCNTL_H
#include <bits/fcntl.h>

#include "libvhd.h"
#include "partition.h"

#define _ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

#define __RESOLVE(func, name)				                \
	do {								\
		if (!_libvhd_io_initialized)				\
			_libvhd_io_init();				\
		if (!(func))						\
			(func) = _get_std_fn((name));			\
	} while (0)

#define _RESOLVE(func) __RESOLVE((func), __func__)

#define LIBVHD_IO_DEBUG "LIBVHD_IO_DEBUG"
#define LIBVHD_IO_DUMP  "LIBVHD_IO_DUMP"
#define LIBVHD_IO_TEST  "LIBVHD_IO_TEST"

static int libvhdio_logging;
static FILE *libvhdio_log;
#define LOG(_f, _a...)						        \
	do {								\
		if (libvhdio_logging && libvhdio_log) {			\
			fprintf(libvhdio_log, _f, ##_a);		\
			fflush(libvhdio_log);				\
		}							\
	} while (0)

static int libvhdio_dump;
#define DUMP(_buf, _size)						\
	do {								\
		if (libvhdio_log && libvhdio_dump) {			\
			int i;						\
			LOG("'");					\
			for (i = 0; i < (_size); i++)			\
				fputc(((char *)(_buf))[i],		\
				      libvhdio_log);			\
			LOG("'\n");					\
		}							\
	} while (0)

struct _function {
	const char                    *name;
	void                          *fn;
};

struct vhd_object {
	vhd_context_t                  vhd;
	int                            refcnt;
	uint64_t                       ino;
	struct list_head               next;
};

struct vhd_partition {
	struct vhd_object             *vhd_obj;
	int                            partition;
	int                            flags;
	off64_t                        start;     /* in sectors */
	off64_t                        end;       /* in sectors */
	off64_t                        size;      /* in sectors */
};

struct vhd_fd_context {
	struct vhd_partition           vhd_part;
	off64_t                        off;
	int                            users;
};

typedef struct vhd_object vhd_object_t;
typedef struct vhd_partition vhd_partition_t;
typedef struct vhd_fd_context vhd_fd_context_t;
typedef int (*_std_open_t)(const char *, int, int);
typedef int (*_std_close_t)(int);
typedef FILE *(*_std_fopen_t)(const char *, const char *);

static struct _function _function_table[] = {
	{ .name = "open",             .fn = NULL },
	{ .name = "open64",           .fn = NULL },
#ifdef __open_2
	{ .name = "__open_2",         .fn = NULL },
#endif // __open_2
#ifdef __open64_2
	{ .name = "__open64_2",       .fn = NULL },
#endif // __open64_2
	{ .name = "close",            .fn = NULL },
	{ .name = "dup",              .fn = NULL },
	{ .name = "dup2",             .fn = NULL },
#ifdef dup3
	{ .name = "dup3",             .fn = NULL },
#endif // dup3
	{ .name = "lseek",            .fn = NULL },
	{ .name = "lseek64",          .fn = NULL },
	{ .name = "read",             .fn = NULL },
	{ .name = "write",            .fn = NULL },
	{ .name = "pread",            .fn = NULL },
	{ .name = "pread64",          .fn = NULL },
	{ .name = "pwrite",           .fn = NULL },
	{ .name = "pwrite64",         .fn = NULL },
	{ .name = "fsync",            .fn = NULL },
	{ .name = "__xstat",          .fn = NULL },
	{ .name = "__xstat64",        .fn = NULL },
	{ .name = "__fxstat",         .fn = NULL },
	{ .name = "__fxstat64",       .fn = NULL },
	{ .name = "__lxstat",         .fn = NULL },
	{ .name = "__lxstat64",       .fn = NULL },
	{ .name = "ioctl",            .fn = NULL },
	{ .name = "fcntl",            .fn = NULL },

	{ .name = "fopen",            .fn = NULL },
	{ .name = "fopen64",          .fn = NULL },
	{ .name = "_IO_getc",         .fn = NULL },
	{ .name = "fread",            .fn = NULL },

	{ .name = "posix_memalign",   .fn = NULL },
};

static int _libvhd_io_interpose = 1;
static struct list_head _vhd_objects;
static vhd_fd_context_t **_vhd_map;
static int _vhd_map_size;

static int _libvhd_io_initialized;
static void _libvhd_io_init(void) __attribute__((constructor));

static volatile sig_atomic_t _libvhd_io_reset_vhds;

static void *
_load_std_fn(const char *name)
{
	void *fn;
	char *msg;

	LOG("loading %s\n", name);

	fn = dlsym(RTLD_NEXT, name);
	msg = dlerror();
	if (!fn || msg) {
		LOG("dlsym '%s' failed: %s\n", name, msg);
		exit(1);
	}

	return fn;
}

static void *
_get_std_fn(const char *name)
{
	int i;

	for (i = 0; i < _ARRAY_SIZE(_function_table); i++)
		if (!strcmp(name, _function_table[i].name))
			return _function_table[i].fn;

	return NULL;
}

static void
_init_vhd_log(void)
{
	int (*_std_dup)(int) = _load_std_fn("dup");
	int log_fd = _std_dup(STDERR_FILENO);

	libvhdio_log = fdopen(log_fd, "a");

	if (getenv(LIBVHD_IO_DEBUG)) {
		libvhdio_logging = 1;
		libvhd_set_log_level(1);
	}

	if (getenv(LIBVHD_IO_DUMP))
		libvhdio_dump = 1;
}

static void
_init_vhd_map(void)
{
	_vhd_map_size = sysconf(_SC_OPEN_MAX);
	_vhd_map = calloc(_vhd_map_size, sizeof(vhd_fd_context_t *));
	if (!_vhd_map) {
		LOG("failed to init vhd map\n");
		exit(1);
	}
}

static void
_init_vhd_objs(void)
{
	INIT_LIST_HEAD(&_vhd_objects);
}

static void
_libvhd_io_reset(void)
{
	int i, err;

	if (!_libvhd_io_interpose)
		return;

	_libvhd_io_reset_vhds = 0;

	if (!_vhd_map)
		return;

	_libvhd_io_interpose = 0;

	for (i = 0; i < _vhd_map_size; i++) {
		int flags;
		vhd_context_t *vhd;
		char *child, *parent;
		vhd_fd_context_t *vhd_fd = _vhd_map[i];

		if (!vhd_fd)
			continue;

		vhd = &vhd_fd->vhd_part.vhd_obj->vhd;

		flags = vhd->oflags;
		child = strdup(vhd->file);
		if (!child)
			exit(ENOMEM);

		LOG("resetting vhd fd %d user fd %d\n", vhd->fd, i);
		vhd_close(vhd);

		if (asprintf(&parent, "%s.%d.vhd",
			     child, (int)time(NULL)) == -1)
			exit(ENOMEM);

		if (rename(child, parent))
			exit(errno);

		err = vhd_snapshot(child, 0, parent, 0, 0);
		if (err) {
			LOG("snapshot of %s failed on reset: %d\n",
			    child, err);
			exit(1);
		}

		err = vhd_open(vhd, child, flags);
		if (err) {
			LOG("opening new snapshot %s failed on reset: %d\n",
			    child, err);
			exit(1);
		}

		LOG("snapshot %s %s vhd fd %d user fd %d\n",
		    child, parent, vhd->fd, i);

		free(child);
		free(parent);
	}

	_libvhd_io_interpose = 1;
}

static void
_libvhd_io_continue(int signo)
{
	_libvhd_io_reset_vhds = 1;
}

static void
_init_vhd_test(void)
{
	if (getenv(LIBVHD_IO_TEST)) {
		sigset_t set;
		struct sigaction act;

		if (sigemptyset(&set))
			exit(1);

		act = (struct sigaction) {
			.sa_handler  = _libvhd_io_continue,
			.sa_mask     = set,
			.sa_flags    = 0,
		};

		if (sigaction(SIGCONT, &act, NULL)) {
			LOG("failed to set signal handler: %d\n", errno);
			exit(1);
		}

		LOG("testing enabled\n");
	}
}

static void
_libvhd_io_init(void)
{
	int i;

	if (_libvhd_io_initialized)
		return;

	_init_vhd_log();
	_init_vhd_map();
	_init_vhd_objs();
	_init_vhd_test();

	for (i = 0; i < _ARRAY_SIZE(_function_table); i++)
		_function_table[i].fn = _load_std_fn(_function_table[i].name);

	LOG("\n");
	_libvhd_io_initialized = 1;
}

static vhd_object_t *
_libvhd_io_get_vhd(const char *path, int flags)
{
	struct stat64 st;
	int err, vhd_flags;
	vhd_object_t *tmp, *obj = NULL;

	_libvhd_io_interpose = 0;

	if (stat64(path, &st))
		goto out;

	list_for_each_entry(tmp, &_vhd_objects, next)
		if (tmp->ino == st.st_ino) {
			obj = tmp;
			if (flags & (O_RDWR | O_WRONLY) &&
			    obj->vhd.oflags & VHD_OPEN_RDONLY) {
				errno = EACCES;
				obj = NULL;
			}
			goto out;
		}

	vhd_flags = VHD_OPEN_CACHED;

	/*
	 * we open RDWR whenever we can since vhd objects may be shared and
	 * we don't have a clean way to switch RDONLY vhds to RDWR.  we'll
	 * only open RDONLY when (flags & O_RDONLY) and we lack permission
	 * to open RDWR.
	 */
	if (access(path, W_OK) == -1) {
		if (errno != EACCES)
			goto out;

		if (flags & (O_WRONLY | O_RDWR))
			goto out;

		vhd_flags |= VHD_OPEN_RDONLY;
	} else {
		vhd_flags |= VHD_OPEN_RDWR;
	}

	obj = malloc(sizeof(*obj));
	if (!obj) {
		errno = ENOMEM;
		goto out;
	}

	INIT_LIST_HEAD(&obj->next);
	obj->refcnt = 0;
	obj->ino = st.st_ino;

	err = vhd_open(&obj->vhd, path, vhd_flags);
	if (err) {
		free(obj);
		obj = NULL;
		errno = err;
		goto out;
	}

	list_add(&obj->next, &_vhd_objects);

out:
	_libvhd_io_interpose = 1;
	if (obj) {
		obj->refcnt++;
		LOG("%s: %s 0x%"PRIx64" 0x%x\n",
		    __func__, path, obj->ino, obj->refcnt);
	}
	return obj;
}

static void
_libvhd_io_put_vhd(vhd_object_t *obj)
{
	LOG("%s: 0x%"PRIx64" 0x%x\n", __func__, obj->ino, obj->refcnt - 1);
	if (--obj->refcnt == 0) {
		vhd_close(&obj->vhd);
		list_del(&obj->next);
		free(obj);
	}
}

static inline vhd_fd_context_t *
_libvhd_io_map_get(int idx)
{
	if (_libvhd_io_reset_vhds)
		_libvhd_io_reset();
	return _vhd_map[idx];
}

static inline void
_libvhd_io_map_set(int idx, vhd_fd_context_t *vhd_fd)
{
	vhd_fd->users++;
	_vhd_map[idx] = vhd_fd;
	LOG("mapping 0x%x to %s (0x%x users)\n",
	    idx, vhd_fd->vhd_part.vhd_obj->vhd.file, vhd_fd->users);
}

static inline void
_libvhd_io_map_clear(int idx)
{
	vhd_fd_context_t *vhd_fd;

	if (idx < 0 || idx >= _vhd_map_size)
		return;

	vhd_fd = _vhd_map[idx];
	_vhd_map[idx] = NULL;

	if (vhd_fd) {
		if (--vhd_fd->users == 0) {
			_libvhd_io_put_vhd(vhd_fd->vhd_part.vhd_obj);
			free(vhd_fd);
		}
	}
}

static int
_libvhd_io_read_bytes(vhd_partition_t *vhd_part,
		      void *buf, size_t size, uint64_t off)
{
	int ret;
	vhd_context_t *vhd = &vhd_part->vhd_obj->vhd;

	_libvhd_io_interpose = 0;
	ret = vhd_io_read_bytes(vhd, buf, size, off);
	_libvhd_io_interpose = 1;

	if (ret) {
		LOG("vhd_io_read_bytes %s %p 0x%zx 0x%"PRIx64" failed: %d\n",
		    vhd->file, buf, size, off, ret);
		errno = -ret;
		ret = 1;
	} else {
		LOG("vhd_io_read_bytes %s %p 0x%zx 0x%"PRIx64"\n",
		    vhd->file, buf, size, off);
		DUMP(buf, size);
	}

	return ret;
}

static int
_libvhd_io_write_bytes(vhd_partition_t *vhd_part,
		       const void *buf, size_t size, uint64_t off)
{
	int ret;
	vhd_context_t *vhd = &vhd_part->vhd_obj->vhd;

	_libvhd_io_interpose = 0;
	ret = vhd_io_write_bytes(vhd, (void *)buf, size, off);
	_libvhd_io_interpose = 1;

	if (ret) {
		LOG("vhd_io_write_bytes %s %p 0x%zx 0x%"PRIx64" failed: %d\n",
		    vhd->file, buf, size, off, ret);
		errno = -ret;
		ret = 1;
	} else {
		LOG("vhd_io_write_bytes %s %p 0x%zx 0x%"PRIx64"\n",
		    vhd->file, buf, size, off);
		DUMP(buf, size);
	}

	return ret;
}

/*
 * symlink pathnames like *.vhd[1-4] are treated specially
 */
static int
_libvhd_io_guess_partition(const char *path, int *partition, int *skip)
{
	char *sfx;
	int err, len;
	struct stat64 st;

	*skip      = 0;
	*partition = 0;

	_libvhd_io_interpose = 0;
	err = lstat64(path, &st);
	_libvhd_io_interpose = 1;

	if (err == -1)
		return errno;

	if ((st.st_mode & __S_IFMT) != __S_IFLNK) {
		if (st.st_size < VHD_SECTOR_SIZE)
			*skip = 1;
		return 0;
	}

	sfx = strstr(path, ".vhd");
	if (!sfx)
		return 0;

	sfx += strlen(".vhd");
	len = strlen(sfx);
	if (!len)
		return 0;
	if (len > 1)
		return EINVAL;

	switch (*sfx) {
	case '1' ... '4':
		*partition = atoi(sfx);
		break;
	default:
		return EINVAL;
	}

	return 0;
}

static int
_libvhd_io_init_partition(vhd_partition_t *vhd_part, int partition)
{
	int err;
	vhd_context_t *vhd;
	void *_p;
	struct partition_table *pt;
	struct primary_partition *p;

	if (partition < 0 || partition > 4)
		return ENOENT;

	vhd = &vhd_part->vhd_obj->vhd;

	if (!partition) {
		vhd_part->partition = 0;
		vhd_part->start     = 0;
		vhd_part->end       = (vhd->footer.curr_size >> VHD_SECTOR_SHIFT);
		vhd_part->size      = vhd_part->end;
		return 0;
	}

	err = posix_memalign(&_p, VHD_SECTOR_SIZE, VHD_SECTOR_SIZE);
	if (err)
		return err;
	pt = _p;

	err = _libvhd_io_read_bytes(vhd_part, pt, 512, 0);
	if (err) {
		LOG("reading partition failed: %d\n", err);
		goto out;
	}

	partition_table_in(pt);
	err = partition_table_validate(pt);
	if (err) {
		LOG("bad partition table read\n");
		goto out;
	}

	p = pt->partitions + (partition - 1);
	if (!p->lba || !p->blocks) {
		err = ENOENT;
		goto out;
	}

	vhd_part->partition = partition;
	vhd_part->start     = p->lba;
	vhd_part->end       = p->lba + p->blocks;
	vhd_part->size      = p->blocks;
	err                 = 0;

	LOG("%s: opening %s partition 0x%x start 0x%08"PRIx64" end 0x%08"PRIx64"\n",
	    __func__, vhd->file, partition, vhd_part->start, vhd_part->end);

out:
	free(pt);
	return err;
}

static int
_libvhd_io_vhd_open(vhd_partition_t *vhd_part, const char *path, int flags)
{
	int err, skip, partition;

	memset(vhd_part, 0, sizeof(*vhd_part));
	vhd_part->flags = flags;

	err = _libvhd_io_guess_partition(path, &partition, &skip);
	if (err)
		return err;

	if (skip)
		return EINVAL;

	LOG("%s: attempting vhd_open of %s\n", __func__, path);

	vhd_part->vhd_obj = _libvhd_io_get_vhd(path, flags);
	if (!vhd_part->vhd_obj)
		err = errno;

	if (!err) {
		err = _libvhd_io_init_partition(vhd_part, partition);
		if (err) {
			_libvhd_io_put_vhd(vhd_part->vhd_obj);
			memset(vhd_part, 0, sizeof(*vhd_part));
		}
	}

	return (err >= 0 ? err : -err);
}

static int
_libvhd_io_open(const char *pathname,
		int flags, mode_t mode, _std_open_t _std_open)
{
	int err, fd;
	vhd_fd_context_t *vhd_fd;

	errno    = 0;
	vhd_fd   = NULL;

	vhd_fd = calloc(1, sizeof(*vhd_fd));
	if (!vhd_fd) {
		err = ENOMEM;
		goto fail;
	}

	err = _libvhd_io_vhd_open(&vhd_fd->vhd_part, pathname, flags);
	if (err) {
		if (err == EINVAL || err == ENOENT)
			goto std_open;

		LOG("%s: vhd_open of %s failed: %d\n",
		    __func__, pathname, err);
		goto fail;
	}

#ifdef O_CLOEXEC
	if (flags & (O_APPEND | O_ASYNC | O_CLOEXEC |
		     O_DIRECTORY | O_NONBLOCK)) {
#else
	if (flags & (O_APPEND | O_ASYNC | O_DIRECTORY | O_NONBLOCK)) {
#endif //O_CLOEXEC
		LOG("%s: invalid flags for vhd_open: 0x%x\n", __func__, flags);
		err = EINVAL;
		goto fail;
	}

	fd = _std_open("/dev/null", O_RDONLY, 0);
	if (fd == -1) {
		err = errno;
		goto fail;
	}

	_libvhd_io_map_set(fd, vhd_fd);
	return fd;

std_open:
	free(vhd_fd);
	return _std_open(pathname, flags, mode);

fail:
	if (vhd_fd && vhd_fd->vhd_part.vhd_obj)
		_libvhd_io_put_vhd(vhd_fd->vhd_part.vhd_obj);
	free(vhd_fd);
	errno = err;
	return -1;
}

static int
_libvhd_io_close(int fd, _std_close_t _std_close)
{
	_libvhd_io_map_clear(fd);
	return _std_close(fd);
}

static FILE *
_libvhd_io_fopen(const char *path, const char *mode)
{
	char *m;
	FILE *f;
	int fd, flags;
	vhd_fd_context_t *vhd_fd;
	static _std_open_t _std_open64;

	__RESOLVE(_std_open64, "open64");

	flags = 0;
	if (strchr(mode, 'a')) {
		if (strchr(mode, '+'))
			flags |= O_APPEND | O_RDWR;
		else
			flags |= O_APPEND | O_WRONLY;
	}
	if (strchr(mode, 'r')) {
		if (strchr(mode, '+'))
			flags |= O_RDWR;
		else
			flags |= O_RDONLY;
	}
	if (strchr(mode, 'w')) {
		errno = EINVAL;
		return NULL;
	}

	fd = _libvhd_io_open(path, flags, 0, _std_open64);
	if (fd == -1)
		return NULL;

	vhd_fd = _libvhd_io_map_get(fd);
	if (vhd_fd)
		m = "r";
	else
		m = (char *)mode;

	f = fdopen(fd, m);
	if (!f) {
		int err = errno;
		close(fd);
		errno = err;
	}

	return f;
}

static ssize_t
_libvhd_io_pread(vhd_partition_t *vhd_part,
		 void *buf, size_t count, off64_t offset)
{
	ssize_t ret;
	off64_t psize;

	ret   = (ssize_t)-1;
	psize = vhd_part->size << VHD_SECTOR_SHIFT;

	if (vhd_part->flags & O_WRONLY) {
		errno = EPERM;
		goto out;
	}

	if (offset >= psize) {
		ret = 0;
		goto out;
	}

	count   = MIN(count, psize - offset);
	offset += (vhd_part->start << VHD_SECTOR_SHIFT);

	if (_libvhd_io_read_bytes(vhd_part, buf, count, offset))
		goto out;

	ret = count;

out:
	return ret;
}

static ssize_t
_libvhd_io_pwrite(vhd_partition_t *vhd_part,
		  const void *buf, size_t count, off64_t offset)
{
	ssize_t ret;
	off64_t psize;

	ret   = (ssize_t)-1;
	psize = vhd_part->size << VHD_SECTOR_SHIFT;

	if (vhd_part->flags & O_RDONLY) {
		errno = EPERM;
		goto out;
	}

	if (offset >= psize) {
		ret = 0;
		goto out;
	}

	count   = MIN(count, psize - offset);
	offset += (vhd_part->start << VHD_SECTOR_SHIFT);

	if (_libvhd_io_write_bytes(vhd_part, buf, count, offset))
		goto out;

	ret = count;

out:
	return ret;
}

static int
_libvhd_io_fstat(int version, vhd_partition_t *vhd_part, struct stat *stats)
{
	int ret;
	static int (*_std___fxstat)(int, int, struct stat *);

	__RESOLVE(_std___fxstat, "__fxstat");
	ret = _std___fxstat(version, vhd_part->vhd_obj->vhd.fd, stats);
	if (ret)
		return ret;

	/*
	 * emulate block device
	 */
	stats->st_size = 0;
	stats->st_blocks = 0;
	stats->st_blksize = getpagesize();
	stats->st_mode &= ~__S_IFREG;
	stats->st_mode |= __S_IFBLK;

	return 0;
}

static int
_libvhd_io_fstat64(int version,
		   vhd_partition_t *vhd_part, struct stat64 *stats)
{
	int ret;
	static int (*_std___fxstat64)(int, int, struct stat64 *);

	__RESOLVE(_std___fxstat64, "__fxstat64");
	ret = _std___fxstat64(version, vhd_part->vhd_obj->vhd.fd, stats);
	if (ret)
		return ret;

	/*
	 * emulate block device
	 */
	stats->st_size = 0;
	stats->st_blocks = 0;
	stats->st_blksize = getpagesize();
	stats->st_mode &= ~__S_IFREG;
	stats->st_mode |= __S_IFBLK;

	return 0;
}

static int
_libvhd_io_stat(int version, const char *path, struct stat *stats)
{
	int err;
	vhd_partition_t vhd_part;

	err = _libvhd_io_vhd_open(&vhd_part, path, O_RDONLY);
	if (err) {
		errno = (err > 0 ? err : -err);
		return -1;
	}

	err = _libvhd_io_fstat(version, &vhd_part, stats);
	_libvhd_io_put_vhd(vhd_part.vhd_obj);

	return err;
}

static int
_libvhd_io_stat64(int version, const char *path, struct stat64 *stats)
{
	int err;
	vhd_partition_t vhd_part;

	err = _libvhd_io_vhd_open(&vhd_part, path, O_RDONLY);
	if (err) {
		errno = (err > 0 ? err : -err);
		return -1;
	}

	err = _libvhd_io_fstat64(version, &vhd_part, stats);
	_libvhd_io_put_vhd(vhd_part.vhd_obj);

	return err;
}

int
open(const char *pathname, int flags, mode_t _mode)
{
	int fd;
	mode_t mode;
	static _std_open_t _std_open;

	_RESOLVE(_std_open);
	mode = (flags & O_CREAT ? _mode : 0);

	if (!_libvhd_io_interpose)
		return _std_open(pathname, flags, mode);

	fd = _libvhd_io_open(pathname, flags, mode, _std_open);

	LOG("%s %s 0x%x 0x%x: 0x%x\n", __func__, pathname, flags, mode, fd);

	return fd;
}

int
open64(const char *pathname, int flags, mode_t _mode)
{
	int fd;
	mode_t mode;
	static _std_open_t _std_open64;

	_RESOLVE(_std_open64);
	mode = (flags & O_CREAT ? _mode : 0);

	if (!_libvhd_io_interpose)
		return _std_open64(pathname, flags, mode);

	fd = _libvhd_io_open(pathname, flags, mode, _std_open64);

	LOG("%s %s 0x%x 0x%x: 0x%x\n", __func__, pathname, flags, mode, fd);

	return fd;
}

int
__open_2(const char *pathname, int flags, mode_t _mode)
{
	int fd;
	mode_t mode;
	static _std_open_t _std___open_2;

	_RESOLVE(_std___open_2);
	mode = (flags & O_CREAT ? _mode : 0);

	if (!_libvhd_io_interpose)
		return _std___open_2(pathname, flags, mode);

	fd = _libvhd_io_open(pathname, flags, mode, _std___open_2);

	LOG("%s %s 0x%x 0x%x: 0x%x\n", __func__, pathname, flags, mode, fd);

	return fd;
}

int
__open64_2(const char *pathname, int flags, mode_t _mode)
{
	int fd;
	mode_t mode;
	static _std_open_t _std___open64_2;

	_RESOLVE(_std___open64_2);
	mode = (flags & O_CREAT ? _mode : 0);

	if (!_libvhd_io_interpose)
		return _std___open64_2(pathname, flags, mode);

	fd = _libvhd_io_open(pathname, flags, mode, _std___open64_2);

	LOG("%s %s 0x%x 0x%x: 0x%x\n", __func__, pathname, flags, mode, fd);

	return fd;
}

int
close(int fd)
{
	static _std_close_t _std_close;

	_RESOLVE(_std_close);

	LOG("%s 0x%x\n", __func__, fd);

	return _libvhd_io_close(fd, _std_close);
}

int
dup(int oldfd)
{
	int newfd;
	vhd_fd_context_t *vhd_fd;
	static int (*_std_dup)(int);

	_RESOLVE(_std_dup);
	vhd_fd = _libvhd_io_map_get(oldfd);

	LOG("%s 0x%x\n", __func__, oldfd);

	newfd = _std_dup(oldfd);
	if (newfd != -1 && vhd_fd)
		_libvhd_io_map_set(newfd, vhd_fd);

	return newfd;
}

int
dup2(int oldfd, int newfd)
{
	int ret;
	vhd_fd_context_t *vhd_fd;
	static int (*_std_dup2)(int, int);

	_RESOLVE(_std_dup2);
	vhd_fd = _libvhd_io_map_get(oldfd);

	LOG("%s 0x%x 0x%x\n", __func__, oldfd, newfd);

	ret = _std_dup2(oldfd, newfd);
	if (ret != -1 && vhd_fd)
		_libvhd_io_map_set(ret, vhd_fd);

	return ret;
}

int
dup3(int oldfd, int newfd, int flags)
{
	int ret;
	vhd_fd_context_t *vhd_fd;
	static int (*_std_dup3)(int, int, int);

	_RESOLVE(_std_dup3);
	vhd_fd = _libvhd_io_map_get(oldfd);

	LOG("%s 0x%x 0x%x 0x%x\n", __func__, oldfd, newfd, flags);

	/*
	 * TODO: handle O_CLOEXEC...
	 */
	ret = _std_dup3(oldfd, newfd, flags);
	if (ret != -1 && vhd_fd)
		_libvhd_io_map_set(ret, vhd_fd);

	return ret;
}

off_t
lseek(int fd, off_t offset, int whence)
{
	off_t new_off;
	vhd_fd_context_t *vhd_fd;
	static off_t (*_std_lseek)(int, off_t, int);

	_RESOLVE(_std_lseek);
	vhd_fd = _libvhd_io_map_get(fd);

	LOG("%s 0x%x 0x%lx 0x%x\n", __func__, fd, offset, whence);

	if (!vhd_fd)
		return _std_lseek(fd, offset, whence);

	switch (whence) {
	case SEEK_SET:
		new_off = offset;
		break;
	case SEEK_CUR:
		new_off = vhd_fd->off + offset;
		break;
	case SEEK_END:
		new_off = (vhd_fd->vhd_part.size << VHD_SECTOR_SHIFT) + offset;
		break;
	default:
		errno = EINVAL;
		return (off_t)-1;
	}

	if (new_off < 0 ||
	    new_off > vhd_fd->vhd_part.size << VHD_SECTOR_SHIFT) {
		errno = EINVAL;
		return (off_t)-1;
	}

	vhd_fd->off = new_off;
	return vhd_fd->off;
}

off64_t
lseek64(int fd, off64_t offset, int whence)
{
	off64_t new_off;
	vhd_fd_context_t *vhd_fd;
	static off64_t (*_std_lseek64)(int, off64_t, int);

	_RESOLVE(_std_lseek64);
	vhd_fd = _libvhd_io_map_get(fd);

	LOG("%s 0x%x 0x%"PRIx64" 0x%x\n", __func__, fd, offset, whence);

	if (!vhd_fd)
		return _std_lseek64(fd, offset, whence);

	switch (whence) {
	case SEEK_SET:
		new_off = offset;
		break;
	case SEEK_CUR:
		new_off = vhd_fd->off + offset;
		break;
	case SEEK_END:
		new_off = (vhd_fd->vhd_part.size << VHD_SECTOR_SHIFT) + offset;
		break;
	default:
		errno = EINVAL;
		return (off64_t)-1;
	}

	if (new_off < 0 ||
	    new_off > vhd_fd->vhd_part.size << VHD_SECTOR_SHIFT) {
		errno = EINVAL;
		return (off64_t)-1;
	}

	vhd_fd->off = new_off;
	return vhd_fd->off;
}

ssize_t
read(int fd, void *buf, size_t count)
{
	ssize_t ret;
	vhd_fd_context_t *vhd_fd;
	static ssize_t (*_std_read)(int, void *, size_t);

	_RESOLVE(_std_read);
	vhd_fd = _libvhd_io_map_get(fd);

	LOG("%s 0x%x %p 0x%zx\n", __func__, fd, buf, count);

	if (!vhd_fd)
		return _std_read(fd, buf, count);

	ret = _libvhd_io_pread(&vhd_fd->vhd_part, buf, count, vhd_fd->off);
	if (ret != -1)
		vhd_fd->off += count;

	return ret;
}

ssize_t
write(int fd, const void *buf, size_t count)
{
	ssize_t ret;
	vhd_fd_context_t *vhd_fd;
	static ssize_t (*_std_write)(int, const void *, size_t);

	_RESOLVE(_std_write);
	vhd_fd = _libvhd_io_map_get(fd);

	LOG("%s 0x%x %p 0x%zx\n", __func__, fd, buf, count);

	if (!vhd_fd)
		return _std_write(fd, buf, count);

	ret = _libvhd_io_pwrite(&vhd_fd->vhd_part, buf, count, vhd_fd->off);
	if (ret != -1)
		vhd_fd->off += count;

	return ret;
}

ssize_t
pread(int fd, void *buf, size_t count, off_t offset)
{
	vhd_fd_context_t *vhd_fd;
	static ssize_t (*_std_pread)(int, void *, size_t, off_t);

	_RESOLVE(_std_pread);
	vhd_fd = _libvhd_io_map_get(fd);

	LOG("%s 0x%x %p 0x%zx 0x%lx\n", __func__, fd, buf, count, offset);

	if (!vhd_fd)
		return _std_pread(fd, buf, count, offset);

	return _libvhd_io_pread(&vhd_fd->vhd_part, buf, count, offset);
}

ssize_t
pread64(int fd, void *buf, size_t count, off64_t offset)
{
	vhd_fd_context_t *vhd_fd;
	static ssize_t (*_std_pread64)(int, void *, size_t, off64_t);

	_RESOLVE(_std_pread64);
	vhd_fd = _libvhd_io_map_get(fd);

	LOG("%s 0x%x %p 0x%zx 0x%"PRIx64"\n", __func__, fd, buf, count, offset);

	if (!vhd_fd)
		return _std_pread64(fd, buf, count, offset);

	return _libvhd_io_pread(&vhd_fd->vhd_part, buf, count, offset);
}

ssize_t
pwrite(int fd, const void *buf, size_t count, off_t offset)
{
	vhd_fd_context_t *vhd_fd;
	static ssize_t (*_std_pwrite)(int, const void *, size_t, off_t);

	_RESOLVE(_std_pwrite);
	vhd_fd = _libvhd_io_map_get(fd);

	LOG("%s 0x%x %p 0x%zx, 0x%lx\n", __func__, fd, buf, count, offset);

	if (!vhd_fd)
		return _std_pwrite(fd, buf, count, offset);

	return _libvhd_io_pwrite(&vhd_fd->vhd_part, buf, count, offset);
}

ssize_t
pwrite64(int fd, const void *buf, size_t count, off64_t offset)
{
	vhd_fd_context_t *vhd_fd;
	static ssize_t (*_std_pwrite64)(int, const void *, size_t, off64_t);

	_RESOLVE(_std_pwrite64);
	vhd_fd = _libvhd_io_map_get(fd);

	LOG("%s 0x%x %p 0x%zx, 0x%"PRIx64"\n", __func__, fd, buf, count, offset);

	if (!vhd_fd)
		return _std_pwrite64(fd, buf, count, offset);

	return _libvhd_io_pwrite(&vhd_fd->vhd_part, buf, count, offset);
}

int
fsync(int fd)
{
	vhd_fd_context_t *vhd_fd;
	static int (*_std_fsync)(int);

	_RESOLVE(_std_fsync);
	vhd_fd = _libvhd_io_map_get(fd);
	if (!vhd_fd)
		return _std_fsync(fd);

	LOG("%s 0x%x\n", __func__, fd);

	return _std_fsync(vhd_fd->vhd_part.vhd_obj->vhd.fd);
}

int
__xstat(int version, const char *path, struct stat *buf)
{
	int ret;
	static int (*_std___xstat)(int, const char *, struct stat *);

	_RESOLVE(_std___xstat);
	if (!_libvhd_io_interpose)
		return _std___xstat(version, path, buf);

	LOG("%s 0x%x %s %p\n", __func__, version, path, buf);

	ret = _libvhd_io_stat(version, path, buf);
	if (ret)
		ret = _std___xstat(version, path, buf);

	return ret;
}

int
__xstat64(int version, const char *path, struct stat64 *buf)
{
	int ret;
	static int (*_std___xstat64)(int, const char *, struct stat64 *);

	_RESOLVE(_std___xstat64);
	if (!_libvhd_io_interpose)
		return _std___xstat64(version, path, buf);

	LOG("%s 0x%x %s %p\n", __func__, version, path, buf);

	ret = _libvhd_io_stat64(version, path, buf);
	if (ret)
		ret = _std___xstat64(version, path, buf);


	return ret;
}

int
__fxstat(int version, int fd, struct stat *buf)
{
	vhd_fd_context_t *vhd_fd;
	static int (*_std___fxstat)(int, int, struct stat *);

	_RESOLVE(_std___fxstat);
	vhd_fd = _libvhd_io_map_get(fd);

	LOG("%s 0x%x 0x%x %p\n", __func__, version, fd, buf);

	if (vhd_fd)
		return _libvhd_io_fstat(version, &vhd_fd->vhd_part, buf);
	else
		return _std___fxstat(version, fd, buf);
}

int
__fxstat64(int version, int fd, struct stat64 *buf)
{
	vhd_fd_context_t *vhd_fd;
	static int (*_std___fxstat64)(int, int, struct stat64 *);

	_RESOLVE(_std___fxstat64);
	vhd_fd = _libvhd_io_map_get(fd);

	LOG("%s 0x%x 0x%x %p\n", __func__, version, fd, buf);

	if (vhd_fd)
		return _libvhd_io_fstat64(version, &vhd_fd->vhd_part, buf);
	else
		return _std___fxstat64(version, fd, buf);
}

/*
 * NB: symlinks to vhds will be stat'ed rather than lstat'ed.
 */
int
__lxstat(int version, const char *path, struct stat *buf)
{
	int ret;
	static int (*_std___lxstat)(int, const char *, struct stat *);

	_RESOLVE(_std___lxstat);
	if (!_libvhd_io_interpose)
		return _std___lxstat(version, path, buf);

	LOG("%s 0x%x %s %p\n", __func__, version, path, buf);

	ret = _libvhd_io_stat(version, path, buf);
	if (ret)
		ret = _std___lxstat(version, path, buf);

	return ret;
}

/*
 * NB: symlinks to vhds will be stat'ed rather than lstat'ed.
 */
int
__lxstat64(int version, const char *path, struct stat64 *buf)
{
	int ret;
	static int (*_std___lxstat64)(int, const char *, struct stat64 *);

	_RESOLVE(_std___lxstat64);
	if (!_libvhd_io_interpose)
		return _std___lxstat64(version, path, buf);

	LOG("%s 0x%x %s %p\n", __func__, version, path, buf);

	ret = _libvhd_io_stat64(version, path, buf);
	if (ret)
		ret = _std___lxstat64(version, path, buf);

	return ret;
}

#ifdef __x86_64__
#define IOCTL_REQUEST long long
#define IOCTL_REQUEST_FMT "%Lx"
#else
#define IOCTL_REQUEST int
#define IOCTL_REQUEST_FMT "%x"
#endif

int
ioctl(int fd, IOCTL_REQUEST request, char *argp)
{
	vhd_fd_context_t *vhd_fd;
	static int (*_std_ioctl)(int, IOCTL_REQUEST, char *);

	_RESOLVE(_std_ioctl);
	vhd_fd = _libvhd_io_map_get(fd);
	if (!vhd_fd)
		return _std_ioctl(fd, request, argp);

	LOG("%s 0x%x 0x" IOCTL_REQUEST_FMT " %p\n", __func__, fd, request, argp);

#ifdef BLKGETSIZE64
	if (request == BLKGETSIZE64) {
		uint64_t *size = (uint64_t *)argp;
		*size = vhd_fd->vhd_part.size << VHD_SECTOR_SHIFT;
		return 0;
	}
#endif
#ifdef BLKGETSIZE
	if (request == BLKGETSIZE) {
		unsigned long *size = (unsigned long *)argp;
		*size = vhd_fd->vhd_part.size << VHD_SECTOR_SHIFT;
		return 0;
	}
#endif
#ifdef BLKSSZGET
	if (request == BLKSSZGET) {
		int *sec_size = (int *)argp;
		*sec_size = VHD_SECTOR_SIZE;
		return 0;
	}
#endif
#ifdef HDIO_GETGEO
	if (request == HDIO_GETGEO) {
		vhd_context_t *vhd = &vhd_fd->vhd_part.vhd_obj->vhd;
		struct hd_geometry *geo = (struct hd_geometry *)argp;
		geo->heads = GEOM_GET_HEADS(vhd->footer.geometry);
		geo->sectors = GEOM_GET_SPT(vhd->footer.geometry);
		geo->cylinders = GEOM_GET_CYLS(vhd->footer.geometry);
		geo->start = vhd_fd->vhd_part.start;
		return 0;
	}
#endif

	return _std_ioctl(fd, request, argp);
}

int
fcntl(int fd, int cmd, ...)
{
	int real_fd;
	va_list args;
	vhd_fd_context_t *vhd_fd;
	static int (*_std_fcntl)(int, int, ...);

	_RESOLVE(_std_fcntl);

	real_fd = fd;
	vhd_fd = _libvhd_io_map_get(fd);
	if (vhd_fd)
		real_fd = vhd_fd->vhd_part.vhd_obj->vhd.fd;

	LOG("%s 0x%x 0x%x\n", __func__, fd, cmd);

	switch (cmd) {
	case F_GETFD:
	case F_GETFL:
	case F_GETOWN:
	case F_GETSIG:
	case F_GETLEASE:
		LOG("%s 0x%x void\n", __func__, real_fd);
		return _std_fcntl(real_fd, cmd);

	case F_DUPFD:
#ifdef F_DUPFD_CLOEXEC
	case F_DUPFD_CLOEXEC:
#endif // F_DUPFD_CLOEXEC
	case F_SETFD:
	case F_SETFL:
	case F_SETOWN:
	case F_SETSIG:
	case F_SETLEASE:
	case F_NOTIFY:
	{
		long arg;
		va_start(args, cmd);
		arg = va_arg(args, long);
		va_end(args);
		LOG("%s 0x%x long 0x%lx\n", __func__, real_fd, arg);
		return _std_fcntl(real_fd, cmd, arg);
	}

	case F_SETLK:
	case F_SETLKW:
	case F_GETLK:
	{
		struct flock *flk;
		va_start(args, cmd);
		flk = va_arg(args, struct flock *);
		va_end(args);
		LOG("%s 0x%x lock %p\n", __func__, real_fd, flk);
		return _std_fcntl(real_fd, cmd, flk);
	}

#if __WORDSIZE == 32
	case F_SETLK64:
	case F_SETLKW64:
	case F_GETLK64:
	{
		struct flock64 *flk;
		va_start(args, cmd);
		flk = va_arg(args, struct flock64 *);
		va_end(args);
		LOG("%s 0x%x lock64 %p (%p)\n",
		    __func__, real_fd, flk, _std_fcntl);
		return _std_fcntl(real_fd, cmd, flk);
	}
#endif

	default:
		LOG("%s unrecognized cmd\n", __func__);
		errno = EINVAL;
		return -1;
	}
}

FILE *
fopen(const char *path, const char *mode)
{
	FILE *f;
	static _std_fopen_t _std_fopen;

	_RESOLVE(_std_fopen);

	if (!_libvhd_io_interpose || strchr(mode, 'w'))
		return _std_fopen(path, mode);

	f = _libvhd_io_fopen(path, mode);

	LOG("%s %s %s: 0x%x\n", __func__, path, mode, (f ? fileno(f) : -1));

	return f;
}

FILE *
fopen64(const char *path, const char *mode)
{
	FILE *f;
	static _std_fopen_t _std_fopen64;

	_RESOLVE(_std_fopen64);

	if (!_libvhd_io_interpose || strchr(mode, 'w'))
		return _std_fopen64(path, mode);

	f = _libvhd_io_fopen(path, mode);

	LOG("%s %s %s: 0x%x\n", __func__, path, mode, (f ? fileno(f) : -1));

	return f;
}

int
_IO_getc(FILE *f)
{
	int cnt;
	unsigned char c;
	vhd_fd_context_t *vhd_fd;
	static int (*_std__IO_getc)(FILE *);

	_RESOLVE(_std__IO_getc);
	vhd_fd = _libvhd_io_map_get(fileno(f));
	if (!vhd_fd)
		return _std__IO_getc(f);

	LOG("%s %p (0x%x)\n", __func__, f, fileno(f));
	cnt = _libvhd_io_pread(&vhd_fd->vhd_part, &c, sizeof(c), vhd_fd->off);
	if (cnt > 0)
		vhd_fd->off += cnt;

	return (int)c;
}

#ifdef _IO_getc_unlocked
#undef _IO_getc_unlocked
#endif
int
_IO_getc_unlocked(FILE *f)
{
	return _IO_getc(f);
}

size_t
fread(void *buf, size_t size, size_t n, FILE *f)
{
	ssize_t cnt;
	vhd_fd_context_t *vhd_fd;
	static size_t (*_std_fread)(void *, size_t, size_t, FILE *);

	_RESOLVE(_std_fread);
	vhd_fd = _libvhd_io_map_get(fileno(f));
	if (!vhd_fd)
		return _std_fread(buf, size, n, f);

	LOG("%s %p 0x%zx 0x%zx %p (0x%x)\n",
	    __func__, buf, size, n, f, fileno(f));
	cnt = _libvhd_io_pread(&vhd_fd->vhd_part, buf, n * size, vhd_fd->off);
	if (cnt > 0) {
		vhd_fd->off += cnt;
		cnt /= size;
	}

	return cnt;
}

#ifdef fread_unlocked
#undef fread_unlocked
#endif
size_t fread_unlocked(void *buf, size_t size, size_t n, FILE *f)
{
	return fread(buf, size, n, f);
}

/*
 * sigh... preloading with bash causes problems, since bash has its own
 * malloc(), memalign(), and free() functions, but no posix_memalign().
 * this causes problems when libvhd free()'s posix_memalign()'ed memory.
 */
#define _libvhd_power_of_2(x) ((((x) - 1) & (x)) == 0)
int
posix_memalign(void **memptr, size_t alignment, size_t size)
{
	if (!alignment || alignment % sizeof(void *) ||
	    !_libvhd_power_of_2(alignment / sizeof(void *)))
		return EINVAL;

	*memptr = memalign(alignment, size);
	if (!*memptr)
		return ENOMEM;

	return 0;
}
