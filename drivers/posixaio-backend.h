/*
 * Copyright (c) 2020, Citrix Systems, Inc.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef POSIX_AIO_BACKEND_H
#define POSIX_AIO_BACKEND_H

#include "scheduler.h"
#include <aio.h>
#include "io-backend.h"

struct tiocb;
struct tfilter;

struct backend* get_posix_aio_backend();

#endif /*POSIX_AIO_BACKEND_H*/
