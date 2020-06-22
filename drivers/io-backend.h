/*
 * Copyright (c) 2016, Citrix Systems, Inc.
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

#ifndef IO_BACKEND_H
#define IO_BACKEND_H

struct tiocb;
struct tfilter;
typedef void* tqueue;
typedef void (*td_queue_callback_t)(void *arg, struct tiocb *, int err);

struct tiocb {
	td_queue_callback_t   cb;
	void                 *arg;

        void		     *iocb;
	struct tiocb         *next;
};

struct tlist {
	struct tiocb         *head;
	struct tiocb         *tail;
};


typedef void (*debug_queue)(tqueue );
typedef int (*init_queue)(tqueue* , int size, int drv, struct tfilter *);
typedef	void (*free_queue)(tqueue* );
typedef	void (*up_queue)(tqueue , struct tiocb *);
typedef	int  (*submit_all_queue)(tqueue );
typedef	int (*submit_tiocbs_queue)(tqueue );
typedef	void (*prep_tiocb_queue)(struct tiocb *, int, int, char *, size_t,
			long long, td_queue_callback_t, void *);

struct backend {
	debug_queue debug;
	init_queue init;
	free_queue free_queue;
	up_queue queue;
	submit_all_queue submit_all;
	submit_tiocbs_queue submit_tiocbs;
	prep_tiocb_queue prep;
};

#endif /*IO_BACKEND_H*/
