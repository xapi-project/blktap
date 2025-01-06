/*
 * Copyright (c) 2017, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <cmocka.h>
#include <errno.h>

#include <wrappers.h>
#include "control-wrappers.h"
#include "test-suites.h"

#include "tap-ctl.h"
#include "blktap.h"

void *proc_misc_data = NULL;

/*
 * not including " 55 blktap/control\n"
 */
char *basic_proc_misc = 
"200 tun\n"
" 46 dlm_xapi-clusterd-lockspace\n"
" 47 dlm_plock\n"
" 48 dlm-monitor\n"
" 49 dlm-control\n"
"237 loop-control\n"
"236 device-mapper\n"
"130 watchdog\n"
" 50 nvme-fabrics\n"
" 51 memory_bandwidth\n"
" 52 network_throughput\n"
" 53 network_latency\n"
" 54 cpu_dma_latency\n"
"  1 psaux\n"
"183 hw_random\n"
"228 hpet\n"
" 56 xen/hypercall\n"
" 57 xen/privcmd\n"
"227 mcelog\n"
" 58 xen/gntalloc\n"
" 59 xen/gntdev\n"
" 60 xen/evtchn\n"
" 61 xen/xenbus_backend\n"
" 62 xen/xenbus\n"
"235 autofs\n"
" 63 vga_arbiter\n";

FILE *prepare_mock_misc(char *additional_data)
{
	size_t file_size = strlen(basic_proc_misc) + 1;

	if (additional_data) {
		file_size += strnlen(additional_data, 1024);
	}
	proc_misc_data = test_malloc(file_size);
	strncpy(proc_misc_data, basic_proc_misc, file_size);
	if (additional_data) {
		strncpy(proc_misc_data + strlen(basic_proc_misc),
			additional_data, file_size - strlen(basic_proc_misc));
	}
	return fmemopen(proc_misc_data, file_size, "r");
}

void free_mock_misc(void)
{
	test_free(proc_misc_data);
}

void test_tap_ctl_allocate_prep_dir_no_access(void **state)
{
	int result;
	int minor;
	char *devname;

	will_return(__wrap_access, ENOENT);
	expect_string(__wrap_access, pathname, "/run/blktap-control");
	will_return(__wrap_mkdir, EEXIST);
	expect_string(__wrap_mkdir, pathname, "/run");
	will_return(__wrap_mkdir, EACCES);
	expect_string(__wrap_mkdir, pathname, "/run/blktap-control");

	result = tap_ctl_allocate(&minor, &devname);

	assert_int_equal(EACCES, result);
}

/* void test_tap_ctl_allocate_no_device_info(void **state) */
/* { */
/*     int result; */
/*     int minor; */
/*     char *devname; */

/*     FILE *proc_misc = prepare_mock_misc(NULL); */

/*     /\* Prepare Directory *\/ */
/*     will_return(__wrap_access, ENOENT); */
/*     expect_string(__wrap_access, pathname, "/var/run/blktap-control"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/var"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/var/run"); */
/*     will_return(__wrap_mkdir, 0); */
/*     expect_string(__wrap_mkdir, pathname, "/var/run/blktap-control"); */
/*     /\* Check Environment *\/ */
/*     will_return(__wrap_fopen, proc_misc); */
/*     will_return(__wrap_flock, 0); */
/*     expect_value(__wrap_flock, fd, fileno(proc_misc)); */
/*     will_return(__wrap_access, ENOENT); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2/control"); */
/*     /\* Close check environment *\/ */
/*     will_return(__wrap_flock, 0); */
/*     expect_value(__wrap_flock, fd, fileno(proc_misc)); */
/*     expect_value(__wrap_fclose, fp, proc_misc); */

/*     result = tap_ctl_allocate(&minor, &devname); */

/*     free_mock_misc(); */

/*     assert_int_equal(ENOSYS, result); */
/* } */

/* void test_tap_ctl_allocate_make_device_fail(void **state) */
/* { */
/*     int result; */
/*     int minor; */
/*     char *devname; */

/*     FILE *proc_misc = prepare_mock_misc(" 55 blktap/control\n"); */

/*     /\* Prepare Directory *\/ */
/*     will_return(__wrap_access, 0); */
/*     expect_string(__wrap_access, pathname, "/var/run/blktap-control"); */
/*     /\* Check Environment *\/ */
/*     will_return(__wrap_fopen, proc_misc); */
/*     will_return(__wrap_flock, 0); */
/*     expect_value(__wrap_flock, fd, fileno(proc_misc)); */
/*     will_return(__wrap_access, ENOENT); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2/control"); */
/*     /\* Make Device/Prepare Directory*\/ */
/*     will_return(__wrap_access, 0); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_unlink, 0); */
/*     expect_string(__wrap_unlink, pathname, "/dev/xen/blktap-2/control"); */
/*     will_return(__wrap___xmknod, EPERM); */
/*     expect_string(__wrap___xmknod, pathname, "/dev/xen/blktap-2/control"); */
/*     /\* Close check environment *\/ */
/*     will_return(__wrap_flock, 0); */
/*     expect_value(__wrap_flock, fd, fileno(proc_misc)); */
/*     expect_value(__wrap_fclose, fp, proc_misc); */

/*     result = tap_ctl_allocate(&minor, &devname); */

/*     free_mock_misc(); */

/*     assert_int_equal(EPERM, result); */
/* } */

/* void test_tap_ctl_allocate_ring_create_fail(void **state) */
/* { */
/*     int result; */
/*     int minor; */
/*     char *devname = NULL; */
/*     int dev_fd = 12; */

/*     FILE *proc_misc = prepare_mock_misc(" 55 blktap/control\n"); */

/*     /\* Prepare Directory *\/ */
/*     will_return(__wrap_access, 0); */
/*     expect_string(__wrap_access, pathname, "/var/run/blktap-control"); */
/*     /\* Check Environment *\/ */
/*     will_return(__wrap_fopen, proc_misc); */
/*     will_return(__wrap_flock, 0); */
/*     expect_value(__wrap_flock, fd, fileno(proc_misc)); */
/*     will_return(__wrap_access, ENOENT); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2/control"); */
/*     /\* Make Device/Prepare Directory*\/ */
/*     will_return(__wrap_access, 0); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_unlink, 0); */
/*     expect_string(__wrap_unlink, pathname, "/dev/xen/blktap-2/control"); */
/*     will_return(__wrap___xmknod, 0); */
/*     expect_string(__wrap___xmknod, pathname, "/dev/xen/blktap-2/control"); */
/*     /\* Close check environment *\/ */
/*     will_return(__wrap_flock, 0); */
/*     expect_value(__wrap_flock, fd, fileno(proc_misc)); */
/*     expect_value(__wrap_fclose, fp, proc_misc); */
/*     /\* allocate device *\/ */
/*     will_return(__wrap_open, dev_fd); */
/*     expect_string(__wrap_open, pathname, "/dev/xen/blktap-2/control"); */
/*     will_return(__wrap_ioctl, 0); */
/*     expect_value(__wrap_ioctl, fd, dev_fd); */
/*     expect_value(__wrap_ioctl, request, BLKTAP2_IOCTL_ALLOC_TAP); */
/*     will_return(__wrap_close, 0); */
/*     expect_value(__wrap_close, fd, dev_fd); */
/*     /\* Make Device - ring *\/ */
/*     will_return(__wrap_access, ENOENT); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev/xen"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_unlink, 0); */
/*     expect_any(__wrap_unlink, pathname); */
/*     will_return(__wrap___xmknod, EPERM); */
/*     expect_any(__wrap___xmknod, pathname); */
/*     /\* tap-ctl-free *\/ */
/*     will_return(__wrap_open, dev_fd); */
/*     expect_string(__wrap_open, pathname, "/dev/xen/blktap-2/control"); */
/*     will_return(__wrap_ioctl, 0); */
/*     expect_value(__wrap_ioctl, fd, dev_fd); */
/*     expect_value(__wrap_ioctl, request, BLKTAP2_IOCTL_FREE_TAP); */
/*     will_return(__wrap_close, 0); */
/*     expect_value(__wrap_close, fd, dev_fd); */

/*     result = tap_ctl_allocate(&minor, &devname); */

/*     free_mock_misc(); */

/*     assert_int_equal(EPERM, result); */
/* } */

/* void test_tap_ctl_allocate_io_device_fail(void **state) */
/* { */
/*     int result; */
/*     int minor; */
/*     char *devname = NULL; */
/*     int dev_fd = 12; */

/*     FILE *proc_misc = prepare_mock_misc(" 55 blktap/control\n"); */

/*     /\* Prepare Directory *\/ */
/*     will_return(__wrap_access, 0); */
/*     expect_string(__wrap_access, pathname, "/var/run/blktap-control"); */
/*     /\* Check Environment *\/ */
/*     will_return(__wrap_fopen, proc_misc); */
/*     will_return(__wrap_flock, 0); */
/*     expect_value(__wrap_flock, fd, fileno(proc_misc)); */
/*     will_return(__wrap_access, ENOENT); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2/control"); */
/*     /\* Make Device/Prepare Directory*\/ */
/*     will_return(__wrap_access, 0); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_unlink, 0); */
/*     expect_string(__wrap_unlink, pathname, "/dev/xen/blktap-2/control"); */
/*     will_return(__wrap___xmknod, 0); */
/*     expect_string(__wrap___xmknod, pathname, "/dev/xen/blktap-2/control"); */
/*     /\* Close check environment *\/ */
/*     will_return(__wrap_flock, 0); */
/*     expect_value(__wrap_flock, fd, fileno(proc_misc)); */
/*     expect_value(__wrap_fclose, fp, proc_misc); */
/*     /\* allocate device *\/ */
/*     will_return(__wrap_open, dev_fd); */
/*     expect_string(__wrap_open, pathname, "/dev/xen/blktap-2/control"); */
/*     will_return(__wrap_ioctl, 0); */
/*     expect_value(__wrap_ioctl, fd, dev_fd); */
/*     expect_value(__wrap_ioctl, request, BLKTAP2_IOCTL_ALLOC_TAP); */
/*     will_return(__wrap_close, 0); */
/*     expect_value(__wrap_close, fd, dev_fd); */
/*     /\* Make Device - ring *\/ */
/*     will_return(__wrap_access, ENOENT); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev/xen"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_unlink, 0); */
/*     expect_any(__wrap_unlink, pathname); */
/*     will_return(__wrap___xmknod, 0); */
/*     expect_any(__wrap___xmknod, pathname); */

/*     /\* Make Device - io device *\/ */
/*     will_return(__wrap_access, ENOENT); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev/xen"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_unlink, 0); */
/*     expect_any(__wrap_unlink, pathname); */
/*     will_return(__wrap___xmknod, EPERM); */
/*     expect_any(__wrap___xmknod, pathname); */

/*     /\* tap-ctl-free *\/ */
/*     will_return(__wrap_open, dev_fd); */
/*     expect_string(__wrap_open, pathname, "/dev/xen/blktap-2/control"); */
/*     will_return(__wrap_ioctl, 0); */
/*     expect_value(__wrap_ioctl, fd, dev_fd); */
/*     expect_value(__wrap_ioctl, request, BLKTAP2_IOCTL_FREE_TAP); */
/*     will_return(__wrap_close, 0); */
/*     expect_value(__wrap_close, fd, dev_fd); */

/*     result = tap_ctl_allocate(&minor, &devname); */

/*     free_mock_misc(); */

/*     assert_int_equal(EPERM, result); */
/* } */

/* void test_tap_ctl_allocate_success(void **state) */
/* { */
/*     int result; */
/*     int minor; */
/*     char *devname = NULL; */
/*     int dev_fd = 12; */

/*     FILE *proc_misc = prepare_mock_misc(" 55 blktap/control\n"); */

/*     /\* Prepare Directory *\/ */
/*     will_return(__wrap_access, 0); */
/*     expect_string(__wrap_access, pathname, "/var/run/blktap-control"); */
/*     /\* Check Environment *\/ */
/*     will_return(__wrap_fopen, proc_misc); */
/*     will_return(__wrap_flock, 0); */
/*     expect_value(__wrap_flock, fd, fileno(proc_misc)); */
/*     will_return(__wrap_access, ENOENT); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2/control"); */
/*     /\* Make Device/Prepare Directory*\/ */
/*     will_return(__wrap_access, 0); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_unlink, 0); */
/*     expect_string(__wrap_unlink, pathname, "/dev/xen/blktap-2/control"); */
/*     will_return(__wrap___xmknod, 0); */
/*     expect_string(__wrap___xmknod, pathname, "/dev/xen/blktap-2/control"); */
/*     /\* Close check environment *\/ */
/*     will_return(__wrap_flock, 0); */
/*     expect_value(__wrap_flock, fd, fileno(proc_misc)); */
/*     expect_value(__wrap_fclose, fp, proc_misc); */
/*     /\* allocate device *\/ */
/*     will_return(__wrap_open, dev_fd); */
/*     expect_string(__wrap_open, pathname, "/dev/xen/blktap-2/control"); */
/*     will_return(__wrap_ioctl, 0); */
/*     expect_value(__wrap_ioctl, fd, dev_fd); */
/*     expect_value(__wrap_ioctl, request, BLKTAP2_IOCTL_ALLOC_TAP); */
/*     will_return(__wrap_close, 0); */
/*     expect_value(__wrap_close, fd, dev_fd); */
/*     /\* Make Device - ring *\/ */
/*     will_return(__wrap_access, ENOENT); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev/xen"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_unlink, 0); */
/*     expect_any(__wrap_unlink, pathname); */
/*     will_return(__wrap___xmknod, 0); */
/*     expect_any(__wrap___xmknod, pathname); */

/*     /\* Make Device - io device *\/ */
/*     will_return(__wrap_access, ENOENT); */
/*     expect_string(__wrap_access, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev/xen"); */
/*     will_return(__wrap_mkdir, EEXIST); */
/*     expect_string(__wrap_mkdir, pathname, "/dev/xen/blktap-2"); */
/*     will_return(__wrap_unlink, 0); */
/*     expect_any(__wrap_unlink, pathname); */
/*     will_return(__wrap___xmknod, 0); */
/*     expect_any(__wrap___xmknod, pathname); */


/*     result = tap_ctl_allocate(&minor, &devname); */

/*     free_mock_misc(); */

/*     assert_int_equal(0, result); */
/* } */
