/* td.c
 *
 * Tapdisk utility program.
 *
 * (c) 2006 Andrew Warfield and Jake Wires
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#define TAPDISK
#include "tapdisk.h"

#if 1
#define DFPRINTF(_f, _a...) fprintf ( stdout, _f , ## _a )
#else
#define DFPRINTF(_f, _a...) ((void)0)
#endif

#define MAX_NAME_LEN 1000

typedef enum {
	TD_CMD_CREATE    = 0,
	TD_CMD_SNAPSHOT  = 1,
	TD_CMD_COALESCE  = 2,
	TD_CMD_QUERY     = 3,
	TD_CMD_INVALID   = 4
} td_command_t;

struct command {
	td_command_t  id;
	char         *name;
	int           needs_type;
};

struct command commands[TD_CMD_INVALID] = {
	{ .id = TD_CMD_CREATE,   .name = "create",   .needs_type = 1 },
	{ .id = TD_CMD_SNAPSHOT, .name = "snapshot", .needs_type = 1 },
	{ .id = TD_CMD_COALESCE, .name = "coalesce", .needs_type = 1 },
	{ .id =	TD_CMD_QUERY,    .name = "query",    .needs_type = 1 }
};

#define COMMAND_NAMES "{ create | snapshot | coalesce | query }"
#define PLUGIN_TYPES  "{ aio | qcow | ram | vhd | vmdk }"

void 
help(void)
{
	fprintf(stderr, "Tapdisk Utilities: v1.0.0\n");
	fprintf(stderr, "usage: td COMMAND [TYPE] [OPTIONS]\n");
	fprintf(stderr, "COMMAND := %s\n", COMMAND_NAMES);
	fprintf(stderr, "TYPE    := %s\n", PLUGIN_TYPES);
	exit(-1);
}

struct command *
get_command(char *command)
{
	int i, len = strlen(command);

	for (i = 0; i < TD_CMD_INVALID; i++)
		if (!strcmp(command, commands[i].name))
			return &commands[i];

	return NULL;
}

int
get_driver_type(char *type)
{
	int i, types, len;

	len   = strlen(type);
	types = sizeof(dtypes)/sizeof(disk_info_t *);

	for (i = 0; i < types; i++)
		if (!strcmp(type, dtypes[i]->handle)) {
			if (dtypes[i]->idnum == DISK_TYPE_SYNC ||
			    dtypes[i]->idnum == DISK_TYPE_VHDSYNC)
				return -1;
			return i;
		}

	return -1;
}

int
init_disk_driver(struct disk_driver *dd, int type, char *name, td_flag_t flags)
{
	int err = -ENOMEM;

	memset(dd, 0, sizeof(struct disk_driver));

	dd->drv      = dtypes[type]->drv;
	dd->private  = malloc(dd->drv->private_data_size);
	dd->td_state = malloc(sizeof(struct td_state));

	if (!dd->td_state || !dd->private)
		goto fail;

	if (name) {
		err = dd->drv->td_open(dd, name, flags);
		if (err)
			goto fail;
	}

	return 0;

 fail:
	free(dd->private);
	free(dd->td_state);
	return -err;
}

inline void
free_disk_driver(struct disk_driver *dd)
{
	dd->drv->td_close(dd);
	free(dd->private);
	free(dd->td_state);
}

int
td_create(int type, int argc, char *argv[])
{
	uint64_t size;
	char *name, *buf;
	int c, i, fd, pagesize, sparse = 1;

	if (type == DISK_TYPE_VMDK) {
		fprintf(stderr, "vmdk create not supported\n");
		return EINVAL;
	}

	while ((c = getopt(argc, argv, "hr")) != -1) {
		switch(c) {
		case 'r':
			sparse = 0;
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
		case 'h':
			goto usage;
		}
	}

	if (optind != (argc - 2))
		goto usage;

	size     = atoi(argv[optind++]);
	size     = size << 20;
	name     = argv[optind];
	pagesize = getpagesize();

	if (strnlen(name, MAX_NAME_LEN) == MAX_NAME_LEN) {
		fprintf(stderr, "Device name too long\n");
		return ENAMETOOLONG;
	}

	/* image-specific create */
	if (dtypes[type]->drv->td_create) {
		td_flag_t flags = (sparse ? TD_SPARSE : 0);
		return dtypes[type]->drv->td_create(name, size, flags);
	}

	/* generic create */
	if (sparse) {
		fprintf(stderr, "Cannot create sparse %s image\n",
			dtypes[type]->handle);
		return EINVAL;
	}

	if (type == DISK_TYPE_RAM &&
	    size > (MAX_RAMDISK_SIZE << SECTOR_SHIFT)) {
		fprintf(stderr, "Max ram disk size is %dMB\n",
			MAX_RAMDISK_SIZE >> (20 - SECTOR_SHIFT));
		return EINVAL;
	}

	buf = calloc(1, pagesize);
	if (!buf)
		return ENOMEM;

	fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd == -1)
		return errno;

	for (i = 0; i < size; i += pagesize)
		if (write(fd, buf, pagesize) != pagesize) {
			close(fd);
			unlink(name);
			free(buf);
			return EIO;
		}

	close(fd);
	free(buf);
	return 0;

 usage:
	fprintf(stderr, "usage: td create %s [-h help] [-r reserve] "
		"<SIZE(MB)> <FILENAME>\n", dtypes[type]->handle);
	return EINVAL;
}

int
td_snapshot(int type, int argc, char *argv[])
{
	int c;
	struct stat stats;
	char *name, *backing;
	struct disk_id pid;

	if (!dtypes[type]->drv->td_snapshot) {
		fprintf(stderr, "Cannot create snapshot of %s image type\n",
			dtypes[type]->handle);
		return EINVAL;
	}

	while ((c = getopt(argc, argv, "h")) != -1) {
		switch(c) {
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
		case 'h':
			goto usage;
		}
	}

	if (optind != (argc - 2))
		goto usage;

	name    = argv[optind++];
	backing = argv[optind++];

	if (strnlen(name, MAX_NAME_LEN) == MAX_NAME_LEN ||
	    strnlen(backing, MAX_NAME_LEN) == MAX_NAME_LEN) {
		fprintf(stderr, "Device name too long\n");
		return ENAMETOOLONG;
	}

	if (stat(backing, &stats) == -1) {
		fprintf(stderr, "File %s not found\n", backing);
		return errno;
	}

	pid.name       = backing;
	pid.drivertype = type;
	return dtypes[type]->drv->td_snapshot(&pid, name, 0);

 usage:
	fprintf(stderr, "usage: td snapshot %s [-h help] "
		"<FILENAME> <BACKING_FILENAME>\n", dtypes[type]->handle);
	return EINVAL;
}

int
td_coalesce(int type, int argc, char *argv[])
{
	char *name;
	int c, pid, ret;
	struct disk_id id;
	struct disk_driver dd;

	if (type != DISK_TYPE_QCOW && type != DISK_TYPE_VHD) {
		fprintf(stderr, "Cannot coalesce images of type %s\n",
			dtypes[type]->handle);
		return EINVAL;
	}

	while ((c = getopt(argc, argv, "h")) != -1) {
		switch(c) {
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
		case 'h':
			goto usage;
		}
	}

	if (optind != (argc - 1))
		goto usage;

	name = argv[optind++];

	if (strnlen(name, MAX_NAME_LEN) == MAX_NAME_LEN) {
		fprintf(stderr, "Device name too long\n");
		return ENAMETOOLONG;
	}

	ret = init_disk_driver(&dd, type, name, TD_RDONLY);
	if (ret) {
		DFPRINTF("Failed opening %s\n", name);
		return ret;
	}
	ret = dd.drv->td_get_parent_id(&dd, &id);
	free_disk_driver(&dd);
	
	if (ret == TD_NO_PARENT) {
		printf("%s has no parent\n", name);
		return EINVAL;
	} else if (ret) {
		printf("Failed getting %s's parent\n", name);
		return EIO;
	}

	pid = fork();
	if (pid == -1) {
		printf("Error forking coalesce process: %d\n", errno);
		return errno;
	} if (pid == 0) {
		switch (type) {
		case DISK_TYPE_QCOW:
			ret = qcow_coalesce(name);
			break;
		case DISK_TYPE_VHD:
			ret = vhd_coalesce(name);
			break;
		default:
			ret = -EINVAL;
		}
		printf("Coalesce returned with %d\n", -ret);
		exit(ret);
	}

	printf("Started coalesce, pid = %d\n", pid);
	return 0;

 usage:
	fprintf(stderr, "usage: td coalesce %s [-h help] "
		"<FILENAME>\n", dtypes[type]->handle);
	return EINVAL;
}

int
td_query(int type, int argc, char *argv[])
{
	char *name;
	int c, size = 0, parent = 0, err = 0;
	struct disk_driver dd;

	while ((c = getopt(argc, argv, "hvp")) != -1) {
		switch(c) {
		case 'v':
			size = 1;
			break;
		case 'p':
			parent = 1;
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
		case 'h':
			goto usage;
		}
	}

	if (optind != (argc - 1))
		goto usage;

	name = argv[optind++];

	if (strnlen(name, MAX_NAME_LEN) == MAX_NAME_LEN) {
		fprintf(stderr, "Device name too long\n");
		return ENAMETOOLONG;
	}

	err = init_disk_driver(&dd, type, name, TD_RDONLY);
	if (err) {
		DFPRINTF("Failed opening %s\n", name);
		return err;
	}

	if (size)
		printf("%llu\n", dd.td_state->size >> 11);
	if (parent) {
		struct disk_id id;
		err = dd.drv->td_get_parent_id(&dd, &id);
		if (!err) {
			printf("%s\n", id.name);
			free(id.name);
		} else if (err == TD_NO_PARENT) {
			printf("%s has no parent\n", name);
			err = EINVAL;
		} else
			printf("query failed\n");
	}

	free_disk_driver(&dd);
	return err;

 usage:
	fprintf(stderr, "usage: td query %s [-h help] [-v virtsize] "
		"[-p parent] <FILENAME>\n", dtypes[type]->handle);
	return EINVAL;
}

int
main(int argc, char *argv[])
{
	char **cargv;
	struct command *cmd;
	int cargc, i, type = -1, ret = 0;

	if (argc < 2)
		help();

	cargc = argc -1;
	cmd   = get_command(argv[1]);
	if (!cmd) {
		fprintf(stderr, "invalid COMMAND %s\n", argv[1]);
		help();
	}

	if (cmd->needs_type) {
		if (argc < 3) {
			fprintf(stderr, "td %s requires a TYPE: %s\n", 
				cmd->name, PLUGIN_TYPES);
			exit(-1);
		}

		type = get_driver_type(argv[2]);
		if (type == -1) {
			fprintf(stderr, "invalid TYPE '%s'.  Choose from: %s\n", 
				argv[2], PLUGIN_TYPES);
			exit(-1);
		}
		--cargc;
	}

	cargv = malloc(sizeof(char *) * cargc);
	if (!cargv)
		exit(ENOMEM);

	for (i = argc - cargc; i < argc; i++)
		cargv[i - (argc - cargc)] = argv[i];

	switch(cmd->id) {
	case TD_CMD_CREATE:
		ret = td_create(type, cargc, cargv);
		break;
	case TD_CMD_SNAPSHOT:
		ret = td_snapshot(type, cargc, cargv);
		break;
	case TD_CMD_COALESCE:
		ret = td_coalesce(type, cargc, cargv);
		break;
	case TD_CMD_QUERY:
		ret = td_query(type, cargc, cargv);
		break;
	case TD_CMD_INVALID:
		ret = EINVAL;
		break;
	}

	free(cargv);

	return (ret >= 0 ? ret : -ret);
}
