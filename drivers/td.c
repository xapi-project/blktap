/* td.c
 *
 * Tapdisk utility program.
 *
 * XenSource proprietary code.
 *
 * Copyright (c) 2007, XenSource Inc.
 * All rights reserved.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <string.h>

#define TAPDISK
#include "tapdisk.h"
#include "vhd.h"

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
	TD_CMD_SET       = 4,
	TD_CMD_REPAIR    = 5,
	TD_CMD_FILL      = 6,
	TD_CMD_READ      = 7,
	TD_CMD_INVALID   = 8
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
	{ .id =	TD_CMD_QUERY,    .name = "query",    .needs_type = 1 },
	{ .id = TD_CMD_SET,      .name = "set",      .needs_type = 1 },
	{ .id = TD_CMD_REPAIR,   .name = "repair",   .needs_type = 1 },
	{ .id = TD_CMD_FILL,     .name = "fill",     .needs_type = 1 },
	{ .id = TD_CMD_READ,     .name = "read",     .needs_type = 1 },
};

#define COMMAND_NAMES "{ create | snapshot | coalesce | query | set | repair | fill | read }"
#define PLUGIN_TYPES  "{ aio | qcow | ram | vhd | vmdk }"

#define print_field_names()                                           \
do {                                                                  \
	int i;                                                        \
	fprintf(stderr, "FIELD := { ");                               \
	fprintf(stderr, "%s", td_vdi_fields[0].name);                 \
	for (i = 1; i < TD_FIELD_INVALID; i++)                        \
		fprintf(stderr, " | %s", td_vdi_fields[i].name);      \
	fprintf(stderr, " }\n");                                      \
} while (0)

void 
help(void)
{
	fprintf(stderr, "Tapdisk Utilities: v1.0.0\n");
	fprintf(stderr, "usage: td-util COMMAND [TYPE] [OPTIONS]\n");
	fprintf(stderr, "COMMAND := %s\n", COMMAND_NAMES);
	fprintf(stderr, "TYPE    := %s\n", PLUGIN_TYPES);
	exit(-1);
}

struct command *
get_command(char *command)
{
	int i;

	for (i = 0; i < TD_CMD_INVALID; i++)
		if (!strcmp(command, commands[i].name))
			return &commands[i];

	return NULL;
}

struct vdi_field *
get_field(char *field)
{
	int i;

	for (i = 0; i < TD_FIELD_INVALID; i++)
		if (!strcmp(field, td_vdi_fields[i].name))
			return &td_vdi_fields[i];

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
			    dtypes[i]->idnum == DISK_TYPE_VHDSYNC ||
			    !dtypes[i]->drv)
				return -1;
			return i;
		}

	return -1;
}

int
namedup(char **dup, char *name)
{
	*dup = NULL;

	if (strnlen(name, MAX_NAME_LEN) >= MAX_NAME_LEN)
		return ENAMETOOLONG;
	
	*dup = strdup(name);
	return 0;
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
		err = namedup(&dd->name, name);
		if (err)
			goto fail;

		err = dd->drv->td_open(dd, name, flags);
		if (err)
			goto fail;
	}

	return 0;

 fail:
	free(dd->name);
	free(dd->private);
	free(dd->td_state);
	return -err;
}

inline void
free_disk_driver(struct disk_driver *dd)
{
	dd->drv->td_close(dd);
	free(dd->name);
	free(dd->private);
	free(dd->td_state);
}

int
td_create(int type, int argc, char *argv[])
{
	ssize_t mb;
	uint64_t size;
	char *name, *buf;
	int c, i, fd, sparse = 1;

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

	mb   = 1 << 20;
	size = atoi(argv[optind++]);
	size = size << 20;
	name = argv[optind];

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

	buf = calloc(1, mb);
	if (!buf)
		return ENOMEM;

	fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd == -1)
		return errno;

	size >>= 20;
	for (i = 0; i < size; i++)
		if (write(fd, buf, mb) != mb) {
			close(fd);
			unlink(name);
			free(buf);
			return EIO;
		}

	close(fd);
	free(buf);
	return 0;

 usage:
	fprintf(stderr, "usage: td-util create %s [-h help] [-r reserve] "
		"<SIZE(MB)> <FILENAME>\n", dtypes[type]->handle);
	return EINVAL;
}


/* 
 * search a chain of vhd snapshots for
 * the first image that has been written to 
 */
int
get_non_zero_image(char **image, int type, char *backing)
{
	int i, err;
	char *name;
	struct disk_id pid;
	struct vhd_info info;
	struct disk_driver dd;

	if (type != DISK_TYPE_VHD)
		return namedup(image, backing);

	*image         = NULL;
	name           = backing;
	pid.drivertype = type;

	do {
		err = init_disk_driver(&dd, type, name, TD_RDONLY);

		if (name != backing)
			free(name);

		if (err)
			return err;

		err = dd.drv->td_get_parent_id(&dd, &pid);
		if (err) {
			if (err == TD_NO_PARENT)
				err = namedup(image, dd.name);
			free_disk_driver(&dd);
			return err;
		}

		err = vhd_get_info(&dd, &info);
		if (err) {
			free_disk_driver(&dd);
			free(pid.name);
			return err;
		}

		for (i = 0; i < info.bat_entries; i++)
			if (info.bat[i] != DD_BLK_UNUSED) {
				err = namedup(image, dd.name);
				free(pid.name);
				pid.name = NULL;
				break;
			}

		name = pid.name;
		free(info.bat);
		free_disk_driver(&dd);

	} while(!*image && !err);

	return err;
}

int
td_snapshot(int type, int argc, char *argv[])
{
	int c, err;
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

	pid.drivertype = type;
	err = get_non_zero_image(&pid.name, type, backing);
	if (err)
		return err;

	err = dtypes[type]->drv->td_snapshot(&pid, name, 0);

	free(pid.name);
	return err;

 usage:
	fprintf(stderr, "usage: td-util snapshot %s [-h help] "
		"<FILENAME> <BACKING_FILENAME>\n", dtypes[type]->handle);
	return EINVAL;
}

int
td_coalesce(int type, int argc, char *argv[])
{
	char *name;
	int c, ret;
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

	switch (type) {
	case DISK_TYPE_VHD:
		ret = vhd_coalesce(name);
		break;
	case DISK_TYPE_QCOW:
		/* ret = qcow_coalesce(name); */
	default:
		ret = -EINVAL;
	}

	if (ret)
		printf("coalesce failed: %d\n", -ret);

	return ret;

 usage:
	fprintf(stderr, "usage: td-util coalesce %s [-h help] "
		"<FILENAME>\n", dtypes[type]->handle);
	return EINVAL;
}

int
td_query(int type, int argc, char *argv[])
{
	char *name;
	int c, size = 0, parent = 0, fields = 0, err = 0;
	struct disk_driver dd;

	while ((c = getopt(argc, argv, "hvpf")) != -1) {
		switch(c) {
		case 'v':
			size = 1;
			break;
		case 'p':
			parent = 1;
			break;
		case 'f':
			if (type != DISK_TYPE_VHD && type != DISK_TYPE_QCOW) {
				fprintf(stderr, "Cannot read fields of %s "
					"images\n", dtypes[type]->handle);
				return EINVAL;
			}
			fields = 1;
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

	err = init_disk_driver(&dd, type, name, TD_RDONLY | TD_QUIET);
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
			err = ENODATA;
		} else
			printf("query failed\n");
	}
	if (fields) {
		int i;
		long *values;
		struct vhd_info vinfo;
		/* struct qcow_info qinfo; */

		switch(type) {
		case DISK_TYPE_VHD:
			err = vhd_get_info(&dd, &vinfo);
			values = vinfo.td_fields;
			free(vinfo.bat);
			break;
		case DISK_TYPE_QCOW:
			/*
			err = qcow_get_info(&dd, &qinfo);
			values = qinfo.td_fields;
			free(qinfo.l1);
			if (!err && !qinfo.valid_td_fields)
				err = EINVAL;
			*/
			break;
		}

		if (!err)
			for (i = 0; i < TD_FIELD_INVALID; i++)
				printf("%s: %ld\n", 
				       td_vdi_fields[i].name, values[i]);
		else
			printf("field query failed\n");
	}

	free_disk_driver(&dd);
	return err;

 usage:
	fprintf(stderr, "usage: td-util query %s [-h help] [-v virtsize] "
		"[-p parent] [-f fields] <FILENAME>\n", dtypes[type]->handle);
	return EINVAL;
}

int
td_set_field(int type, int argc, char *argv[])
{
	char *name;
	long value;
	int ret, i, c;
	struct disk_driver dd;
	struct vdi_field *field;

	if (type != DISK_TYPE_VHD && type != DISK_TYPE_QCOW) {
		fprintf(stderr, "Cannot set fields of %s images\n",
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

	if (optind != (argc - 3))
		goto usage;

	name  = argv[optind++];

	field = get_field(argv[optind]);
	if (!field) {
		fprintf(stderr, "Invalid field %s\n", argv[optind]);
		goto usage;
	}

	errno = 0;
	value = strtol(argv[++optind], NULL, 10);
	if (errno) {
		fprintf(stderr, "Invalid value %s\n", argv[optind]);
		goto usage;
	}

	ret = init_disk_driver(&dd, type, name, 0);
	if (ret) {
		DPRINTF("Failed opening %s\n", name);
		return ret;
	}

	switch(type) {
	case DISK_TYPE_VHD:
		ret = vhd_set_field(&dd, field->id, value);
		break;
	case DISK_TYPE_QCOW:
		/* ret = qcow_set_field(&dd, field->id, value);*/
		break;
	}

	free_disk_driver(&dd);

	return ret;

 usage:
	fprintf(stderr, "usage: td-util set %s [-h help] "
		"<FILENAME> <FIELD> <VALUE>\n", dtypes[type]->handle);
	print_field_names();
	return EINVAL;
}

int
td_repair(int type, int argc, char *argv[])
{
	char *name;
	int ret, c;
	struct disk_driver dd;

	if (type != DISK_TYPE_VHD) {
		fprintf(stderr, "Cannot repair %s images\n",
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
	ret  = init_disk_driver(&dd, type, name, 0);
	if (ret) {
		DPRINTF("Failed opening %s\n", name);
		return ret;
	}

	ret = vhd_repair(&dd);
	if (!ret)
		printf("%s successfully repaired\n", name);
	else
		printf("failed to repair %s: %d\n", name, ret);

	free_disk_driver(&dd);

	return ret;

 usage:
	fprintf(stderr, "usage: td-util repair %s [-h help] "
		"<FILENAME>\n", dtypes[type]->handle);
	return EINVAL;
}

int
td_fill(int type, int argc, char *argv[])
{
	int c, ret;
	char *name;

	if (type != DISK_TYPE_VHD) {
		fprintf(stderr, "Cannot fill images of type %s\n",
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

	ret = vhd_fill(name);
	if (!ret)
		printf("%s successfully filled\n", name);
	else
		printf("failed to fill %s: %d\n", name, ret);

	return ret;

 usage:
	fprintf(stderr, "usage: td-util fill %s [-h help] "
		"<FILENAME>\n", dtypes[type]->handle);
	return EINVAL;
}

int
td_read(int type, int argc, char *argv[])
{
	int err = 0;
	char *name;
	struct disk_driver dd;

	if (argc <= 1)
		goto usage;

	if (type != DISK_TYPE_VHD) {
		fprintf(stderr, "Cannot read %s images\n",
			dtypes[type]->handle);
		return EINVAL;
	}

	name = argv[1];
	if (strnlen(name, MAX_NAME_LEN) == MAX_NAME_LEN) {
		fprintf(stderr, "Device name too long\n");
		err = ENAMETOOLONG;
		goto usage;
	}

	err = init_disk_driver(&dd, type, name, TD_RDONLY | TD_QUIET);
	if (err) {
		DFPRINTF("Failed opening %s\n", name);
		goto usage;
	}

	err = vhd_read(&dd, argc, argv);
	free_disk_driver(&dd);

	return err;

 usage:
	fprintf(stderr, "usage: td-util read %s <FILENAME> "
		"[image-specific options]\n", dtypes[type]->handle);
	return err;
}

int
main(int argc, char *argv[])
{
	char **cargv;
	struct command *cmd;
	int cargc, i, type = -1, ret = 0;

#ifdef CORE_DUMP
	struct rlimit rlim;
	rlim.rlim_cur = RLIM_INFINITY;
	rlim.rlim_max = RLIM_INFINITY;
	if (setrlimit(RLIMIT_CORE, &rlim) < 0)
		fprintf(stderr, "setrlimit failed: %d\n", errno);
#endif

	if (argc < 2)
		help();

	cargc = argc - 1;
	cmd   = get_command(argv[1]);
	if (!cmd) {
		fprintf(stderr, "invalid COMMAND %s\n", argv[1]);
		help();
	}

	if (cmd->needs_type) {
		if (argc < 3) {
			fprintf(stderr, "td-util %s requires a TYPE: %s\n", 
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

	cargv[0] = cmd->name;
	for (i = 1; i < cargc; i++)
		cargv[i] = argv[i + (argc - cargc)];

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
	case TD_CMD_SET:
		ret = td_set_field(type, cargc, cargv);
		break;
	case TD_CMD_REPAIR:
		ret = td_repair(type, cargc, cargv);
		break;
	case TD_CMD_FILL:
		ret = td_fill(type, cargc, cargv);
		break;
	case TD_CMD_READ:
		ret = td_read(type, cargc, cargv);
		break;
	case TD_CMD_INVALID:
		ret = EINVAL;
		break;
	}

	free(cargv);

	return (ret >= 0 ? ret : -ret);
}
