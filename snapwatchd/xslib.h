/*
 * Copyright (C) Citrix Systems Inc.
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

struct int_result
{
	int result;
	int err;
};

struct xs_read_result
{
	int result;
	char *readString;
	int noOfBytesRead;
	int err;
};

struct int_result get_min_blk_size(int fd);
struct int_result open_file_for_write(char *path);
struct int_result open_file_for_read(char *path);
struct int_result xs_file_write(int fd, int offset, int blocksize, char* data, int length);
struct xs_read_result xs_file_read(int fd, int offset, int bytesToRead, int min_block_size);
