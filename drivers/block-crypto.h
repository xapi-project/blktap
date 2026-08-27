/*
 * Copyright (c) 2019, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

int vhd_open_crypto(vhd_context_t *vhd, struct td_vbd_encryption *encryption, const char *name);
void vhd_close_crypto(vhd_context_t *vhd);
void vhd_crypto_encrypt(vhd_context_t *vhd, td_request_t *t, char *orig_buf);
void vhd_crypto_decrypt(vhd_context_t *vhd, td_request_t *t);
