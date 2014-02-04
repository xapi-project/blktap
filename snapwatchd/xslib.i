%module xslib
%{
/* Copyright (C) Citrix Systems Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <xenstore.h>
#include "xslib.h"
%}

%include "xslib.h"

/*Core Xen utilities*/
struct xs_handle *xs_daemon_open(void);
void xs_daemon_close(struct xs_handle *h);
int xs_fileno(struct xs_handle *h);

int remove_base_watch(struct xs_handle *h);
int register_base_watch(struct xs_handle *h);
int xs_exists(struct xs_handle *h, const char *path);
char *getval(struct xs_handle *h, const char *path);
int setval(struct xs_handle *h, const char *path, const char *val);
char *dirlist(struct xs_handle *h, const char *path);
int remove_xs_entry(struct xs_handle *h, char *dom_uuid, char *dom_path);
int generic_remove_xs_entry(struct xs_handle *h, char *path);
char *control_handle_event(struct xs_handle *h);
struct int_result get_min_blk_size(int fd);
struct int_result open_file_for_write(char *path);
struct int_result open_file_for_read(char *path);
struct int_result xs_file_write(int fd, int offset, int blocksize, char* data, int length);
struct xs_read_result xs_file_read(int fd, int offset, int bytesToRead, int min_block_size);
void close_file(int fd);
