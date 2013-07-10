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

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "relative-path.h"
#include "canonpath.h"

#define sfree(ptr)         \
do {                       \
	free(ptr);         \
	ptr = NULL;        \
} while (0)

/*
 * count number of tokens between DELIMETER characters
 */
static int
count_nodes(char *path)
{
	int i;
	char *tmp;

	if (!path)
		return 0;

	for (i = 0, tmp = path; *tmp != '\0'; tmp++)
		if (*tmp == DELIMITER)
			i++;

	return i;
}

/*
 * return copy of next node in @path, or NULL
 * @path is moved to the end of the next node
 * @err is set to -errno on failure
 * copy should be freed
 */
static char *
next_node(char **path, int *err)
{
	int ret;
	char *tmp, *start;

	if (!path || !*path) {
		*err = -EINVAL;
		return NULL;
	}

	*err  = 0;
	start = *path;

	for (tmp = *path; *tmp != '\0'; tmp++)
		if (*tmp == DELIMITER) {
			int size;
			char *node;

			size = tmp - start + 1;
			node = malloc(size);
			if (!node) {
				*err = -ENOMEM;
				return NULL;
			}

			ret = snprintf(node, size, "%s", start);
			if (ret < 0) {
				free(node);
				*err = -EINVAL;
				return NULL;
			}

			*path = tmp;
			return node;
		}

	return NULL;
}

/*
 * count number of nodes in common betwee @to and @from
 * returns number of common nodes, or -errno on failure
 */
static int
count_common_nodes(char *to, char *from)
{
	int err, common;
	char *to_node, *from_node;

	if (!to || !from)
		return -EINVAL;

	err       = 0;
	common    = 0;
	to_node   = NULL;
	from_node = NULL;

	do {
		to_node = next_node(&to, &err);
		if (err || !to_node)
			break;

		from_node = next_node(&from, &err);
		if (err || !from_node)
			break;

		if (strncmp(to_node, from_node, MAX_NAME_LEN))
			break;

		++to;
		++from;
		++common;
		sfree(to_node);
		sfree(from_node);

	} while (1);

	sfree(to_node);
	sfree(from_node);

	if (err)
		return err;

	return common;
}

/*
 * construct path of @count '../', './' if @count is zero, or NULL on error
 * result should be freed
 */
static char *
up_nodes(int count)
{
	char *path, *tmp;
	int i, ret, len, size;

	if (!count)
		return strdup("./");

	len  = strlen("../");
	size = len * count;
	if (size >= MAX_NAME_LEN)
		return NULL;

	path = malloc(size + 1);
	if (!path)
		return NULL;

	tmp = path;
	for (i = 0; i < count; i++) {
		ret = sprintf(tmp, "../");
		if (ret < 0 || ret != len) {
			free(path);
			return NULL;
		}
		tmp += ret;
	}

	return path;
}

/*
 * return pointer to @offset'th node of path or NULL on error
 */
static char *
node_offset(char *from, int offset)
{
	char *path;

	if (!from || !offset)
		return NULL;

	for (path = from; *path != '\0'; path++) {
		if (*path == DELIMITER)
			if (--offset == 0)
				return path + 1;
	}

	return NULL;
}

/*
 * return a relative path from @from to @to
 * result should be freed
 */
char *
relative_path_to(char *from, char *to, int *err)
{
	int from_nodes, common;
	char *to_absolute, __to_absolute[PATH_MAX];
	char *from_absolute, __from_absolute[PATH_MAX];
	char *up, *common_target_path, *relative_path;

	*err          = 0;
	up            = NULL;
	to_absolute   = NULL;
	from_absolute = NULL;
	relative_path = NULL;

	if (strnlen(to, MAX_NAME_LEN)   == MAX_NAME_LEN ||
	    strnlen(from, MAX_NAME_LEN) == MAX_NAME_LEN) {
		EPRINTF("invalid input; max path length is %d\n",
			MAX_NAME_LEN);
		*err = -ENAMETOOLONG;
		return NULL;
	}

	to_absolute = canonpath(to, __to_absolute);
	if (!to_absolute) {
		EPRINTF("failed to get absolute path of %s\n", to);
		*err = -errno;
		goto out;
	}

	from_absolute = canonpath(from, __from_absolute);
	if (!from_absolute) {
		EPRINTF("failed to get absolute path of %s\n", from);
		*err = -errno;
		goto out;
	}

	if (strnlen(to_absolute, MAX_NAME_LEN)   == MAX_NAME_LEN ||
	    strnlen(from_absolute, MAX_NAME_LEN) == MAX_NAME_LEN) {
		EPRINTF("invalid input; max path length is %d\n",
			MAX_NAME_LEN);
		*err = -ENAMETOOLONG;
		goto out;
	}

	/* count nodes in source path */
	from_nodes = count_nodes(from_absolute);

	/* count nodes in common */
	common = count_common_nodes(to_absolute + 1, from_absolute + 1);
	if (common < 0) {
		EPRINTF("failed to count common nodes of %s and %s: %d\n",
			to_absolute, from_absolute, common);
		*err = common;
		goto out;
	}

	/* move up to common node */
	up = up_nodes(from_nodes - common - 1);
	if (!up) {
		EPRINTF("failed to allocate relative path for %s: %d\n",
			from_absolute, -ENOMEM);
		*err = -ENOMEM;
		goto out;
	}

	/* get path from common node to target */
	common_target_path = node_offset(to_absolute, common + 1);
	if (!common_target_path) {
		EPRINTF("failed to find common target path to %s: %d\n",
			to_absolute, -EINVAL);
		*err = -EINVAL;
		goto out;
	}

	/* get relative path */
	if (asprintf(&relative_path, "%s%s", up, common_target_path) == -1) {
		EPRINTF("failed to construct final path %s%s: %d\n",
			up, common_target_path, -ENOMEM);
		relative_path = NULL;
		*err = -ENOMEM;
		goto out;
	}

out:
	sfree(up);

	return relative_path;
}
