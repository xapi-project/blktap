/*
 * Copyright (c) 2020, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LIBAIO_BACKEND_H
#define LIBAIO_BACKEND_H

#include <libaio.h>

#include "io-optimize.h"
#include "scheduler.h"
#include "io-backend.h"

enum {
	TIO_DRV_LIO     = 1,
};

struct backend* get_libaio_backend();

#endif /* LIBAIO_BACKEND_H */
