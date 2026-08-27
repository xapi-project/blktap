/*
 * Copyright (c) 2020, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TAPDISK_PROTOCOL_NEW_H_
#define _TAPDISK_PROTOCOL_NEW_H_

#define NBD_FIXED_SINGLE_EXPORT "tapdisk_client"

#define NBD_REP_ERR(val) (0x80000000 | (val))
#define NBD_MAGIC       UINT64_C(0x4e42444d41474943) /* ASCII "NBDMAGIC" aka INIT_PASSWD */
#define NBD_OLD_VERSION UINT64_C(0x0000420281861253) /* "cliserv_magic" - the NBD old-style magic number */
#define NBD_OPT_MAGIC UINT64_C(0x49484156454F5054) /* ASCII "IHAVEOPT"  */

#define NBD_INFO_EXPORT      0
#define NBD_INFO_NAME        1
#define NBD_INFO_DESCRIPTION 2
#define NBD_INFO_BLOCK_SIZE  3

#define NBD_REP_MAGIC UINT64_C(0x3e889045565a9)

#define NBD_FLAG_FIXED_NEWSTYLE    (1 << 0)
#define NBD_FLAG_NO_ZEROES         (1 << 1)

#define NBD_FLAG_HAS_FLAGS         (1 << 0)
#define NBD_FLAG_READ_ONLY         (1 << 1)
#define NBD_FLAG_SEND_FLUSH        (1 << 2)
#define NBD_FLAG_SEND_FUA          (1 << 3)
#define NBD_FLAG_ROTATIONAL        (1 << 4)
#define NBD_FLAG_SEND_TRIM         (1 << 5)
#define NBD_FLAG_SEND_WRITE_ZEROES (1 << 6)
#define NBD_FLAG_SEND_DF           (1 << 7)
#define NBD_FLAG_CAN_MULTI_CONN    (1 << 8)
#define NBD_FLAG_SEND_CACHE        (1 << 10)
#define NBD_FLAG_SEND_FAST_ZERO    (1 << 11)

#define NBD_OPT_EXPORT_NAME        1
#define NBD_OPT_ABORT              2
#define NBD_OPT_LIST               3
#define NBD_OPT_STARTTLS           5
#define NBD_OPT_INFO               6
#define NBD_OPT_GO                 7
#define NBD_OPT_STRUCTURED_REPLY   8
#define NBD_OPT_LIST_META_CONTEXT  9
#define NBD_OPT_SET_META_CONTEXT   10

#define NBD_STRUCTURED_REPLY_MAGIC  0x668e33ef

#define NBD_REPLY_FLAG_DONE         (1<<0)

#define NBD_REPLY_TYPE_ERR(val) ((1<<15) | (val))
#define NBD_REPLY_TYPE_IS_ERR(val) (!!((val) & (1<<15)))

#define NBD_REPLY_TYPE_NONE         0
#define NBD_REPLY_TYPE_OFFSET_DATA  1
#define NBD_REPLY_TYPE_OFFSET_HOLE  2
#define NBD_REPLY_TYPE_BLOCK_STATUS 5
#define NBD_REPLY_TYPE_ERROR        NBD_REPLY_TYPE_ERR (1)
#define NBD_REPLY_TYPE_ERROR_OFFSET NBD_REPLY_TYPE_ERR (2)

#define base_allocation_id 1

#define NBD_REP_ACK                  1
#define NBD_REP_SERVER               2
#define NBD_REP_INFO                 3
#define NBD_REP_META_CONTEXT         4
#define NBD_REP_ERR_UNSUP            NBD_REP_ERR (1)
#define NBD_REP_ERR_POLICY           NBD_REP_ERR (2)
#define NBD_REP_ERR_INVALID          NBD_REP_ERR (3)
#define NBD_REP_ERR_PLATFORM         NBD_REP_ERR (4)
#define NBD_REP_ERR_TLS_REQD         NBD_REP_ERR (5)
#define NBD_REP_ERR_UNKNOWN          NBD_REP_ERR (6)
#define NBD_REP_ERR_SHUTDOWN         NBD_REP_ERR (7)
#define NBD_REP_ERR_BLOCK_SIZE_REQD  NBD_REP_ERR (8)
#define NBD_REP_ERR_TOO_BIG          NBD_REP_ERR (9)

struct nbd_fixed_new_option_reply_meta_context {
  uint32_t context_id;
} __attribute__((__packed__));

struct nbd_new_handshake {
  uint64_t nbdmagic;
  uint64_t version;
  uint16_t gflags;
} __attribute__((__packed__));

struct nbd_export_name_option_reply {
  uint64_t exportsize;
  uint16_t eflags;
  char zeroes[124];
} __attribute__((__packed__));

struct nbd_new_option {
  uint64_t version;
  uint32_t option;
  uint32_t optlen;
} __attribute__((__packed__));

/** 
 * Define this on the stack when you don't need the data, but use
 * malloc() on a suitable size when you do
 */
struct nbd_fixed_new_option_reply {
  uint64_t magic;
  uint32_t option;
  uint32_t reply;
  uint32_t replylen;
  uint8_t  data[];
} __attribute__((__packed__));

struct nbd_fixed_new_option_rep_server {
  uint32_t namelen;
  char name[];
} __attribute__((__packed__));

struct nbd_fixed_new_option_reply_info_export {
  uint16_t info;
  uint64_t exportsize;
  uint16_t eflags;
} __attribute__((__packed__));

struct nbd_fixed_new_option_reply_info_block_size
{
	uint16_t info; /* NBD_INFO_BLOCK_SIZE */
	uint32_t min_block_size;
	uint32_t preferred_block_size;
	uint32_t max_block_size;
} __attribute__((__packed__));

struct nbd_block_descriptor {
  uint32_t length;
  uint32_t status_flags;
} __attribute__((__packed__));

struct nbd_structured_reply {
  uint32_t magic;
  uint16_t flags;
  uint16_t type;
  uint64_t handle;
  uint32_t length;
} __attribute__((__packed__));

#endif /* _TAPDISK_PROTOCOL_NEW_H_ */

