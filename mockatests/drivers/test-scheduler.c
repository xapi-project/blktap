/*
 * Copyright (c) 2024, Cloud Software Group, Inc.
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

#define _GNU_SOURCE /* Needed for F_GETPIPE_SZ */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/time.h>

#include "scheduler.c"

struct timeval fake_gettimeofday;
int __wrap_gettimeofday(struct timeval* tv, struct timezone* tz)
{
  *tv = fake_gettimeofday;
  return 0;
}

typedef struct {
  int was_called;
  char mode;
  event_id_t id;
} event_cb_spy_t;

void mock_event_cb(event_id_t id, char mode, void *private)
{
  event_cb_spy_t* out = (event_cb_spy_t*)private;
  out->was_called += 1;
  out->mode = mode;
  out->id = id;
}

static int
event_queue_length(const scheduler_t* s)
{
  int len = 0;
  event_t *event;
  scheduler_for_each_event(s, event){
    len += 1;
  }
  return len;
}

void fake_event_cb (event_id_t id, char mode, void *private) {}

int mock_fd_create()
{
  const int fd = eventfd(0, 0);
  if (fd < 1) {
    perror("eventfd");
  }
  assert_true(fd > 0);
  return fd;
}

void mock_fd_set_readable(int fd)
{
  uint64_t b = 1;
  if (write(fd, &b, sizeof(b)) != sizeof(b)) {
    perror("write: ");
    assert_true(false);
  }
}

void mock_fd_set_unwritable(int fd)
{
  uint64_t b = 0xfffffffffffffffe;
  if (write(fd, &b, sizeof(b)) != sizeof(b)) {
    perror("write: ");
    assert_true(false);
  }
}

void mock_fd_set_writable(int fd)
{
  uint64_t b;
  if (read(fd, &b, sizeof(b)) != sizeof(b)){
    perror("read");
    assert_true(false);
  }
}

void
test_scheduler_set_max_timeout(void **state)
{
  scheduler_t s;
  s.max_timeout.tv_sec = 42;
  struct timeval timeout = {.tv_sec = 0 };
  scheduler_set_max_timeout(&s, timeout);
  assert_int_equal(s.max_timeout.tv_sec, 0);
}

void
test_scheduler_set_max_timeout_lower(void **state)
{
  // Setting a new lower value will stick
  scheduler_t s;
  s.max_timeout.tv_sec = 430;
  struct timeval timeout = {.tv_sec = 360 };
  scheduler_set_max_timeout(&s, timeout);
  assert_int_equal(s.max_timeout.tv_sec, 360);
}

void
test_scheduler_set_max_timeout_higher(void **state)
{
  // Setting a new higher value will be ignored
  scheduler_t s;
  s.max_timeout.tv_sec = 430;
  struct timeval timeout = {.tv_sec = 458 };
  scheduler_set_max_timeout(&s, timeout);
  assert_int_equal(s.max_timeout.tv_sec, 430);
}

void
test_scheduler_set_max_timeout_negative(void **state)
{
  // Setting a new negative value will be ignored
  scheduler_t s;
  s.max_timeout.tv_sec = 458;
  struct timeval timeout = {.tv_sec = -2 };
  scheduler_set_max_timeout(&s, timeout);
  assert_int_equal(s.max_timeout.tv_sec, 458);
}

void
test_scheduler_set_max_timeout_inf(void **state)
{
  // Setting TV_INF will be ignored
  scheduler_t s;
  s.max_timeout.tv_sec = 458;
  struct timeval timeout = TV_INF;
  scheduler_set_max_timeout(&s, timeout);
  assert_int_equal(s.max_timeout.tv_sec, 458);
}

void
test_scheduler_register_event_null_callback(void **state)
{
  // Callback cannot be NULL
  scheduler_t s;
  scheduler_initialize(&s);
  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 0;
  struct timeval timeout = {};
  event_cb_t cb = NULL;
  void *private = NULL;

  const int r = scheduler_register_event(&s, mode, fd, timeout, cb, private);
  assert_int_equal(r, -EINVAL);

  scheduler_unregister_event(&s, r);
  scheduler_gc_events(&s);
}

void
test_scheduler_register_event_bad_mode(void **state)
{
  // Bad mode
  scheduler_t s;
  scheduler_initialize(&s);
  char mode = 0;
  int fd = 0;
  struct timeval timeout = {};
  event_cb_t cb = &fake_event_cb;
  void *private = NULL;

  const int r = scheduler_register_event(&s, mode, fd, timeout, cb, private);
  assert_int_equal(r, -EINVAL);

  scheduler_unregister_event(&s, r);
  scheduler_gc_events(&s);
}

void
test_scheduler_register_multiple_events(void **state)
{
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 0;
  struct timeval timeout = {};
  event_cb_t cb = &fake_event_cb;
  void *private = NULL;

  /* Event queue starts off empty */
  assert_int_equal(event_queue_length(&s), 0);

  /* Event queue starts now has one event */
  const int event_id1 = scheduler_register_event(&s, mode, fd, timeout, cb, private);
  assert_int_equal(event_queue_length(&s), 1);

  /* Event queue starts now has two events */
  const int event_id2 = scheduler_register_event(&s, mode, fd, timeout, cb, private);
  assert_int_equal(event_queue_length(&s), 2);

  /* The two event IDs we were given are different */
  assert_int_not_equal(event_id1, event_id2);

  scheduler_unregister_event(&s, event_id1);
  scheduler_unregister_event(&s, event_id2);
  scheduler_gc_events(&s);
}

void
test_scheduler_register_event_populates_event(void **state)
{
  /* scheduler_register_event will fill out all fields in event */
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 458;
  struct timeval timeout = { .tv_sec = 575, .tv_usec = 599 };
  event_cb_t cb = &fake_event_cb;
  int private = 430;

  fake_gettimeofday = (struct timeval){ .tv_sec = 2, .tv_usec = 3};
  const int event_id1 = scheduler_register_event(&s, mode, fd, timeout, cb, &private);

  const event_t* e = list_first_entry(&s.events, event_t, next);

  assert_int_equal(e->id, event_id1);
  assert_int_equal(e->mode, mode);
  assert_int_equal(e->fd, fd);
  assert_int_equal(e->timeout.tv_sec, timeout.tv_sec);
  assert_int_equal(e->timeout.tv_usec, timeout.tv_usec);
  assert_ptr_equal(e->cb, &fake_event_cb);
  assert_ptr_equal(e->private, &private);
  assert_ptr_equal(e->masked, 0);

  assert_int_equal(e->deadline.tv_sec,  fake_gettimeofday.tv_sec + timeout.tv_sec);
  assert_int_equal(e->deadline.tv_usec, fake_gettimeofday.tv_usec + timeout.tv_usec);

  scheduler_unregister_event(&s, event_id1);
  scheduler_gc_events(&s);
}

void
test_scheduler_set_timeout_inf(void **state)
{
  /* If timeout is TV_INF then deadline is TV_INF */
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 458;
  struct timeval timeout = TV_INF;
  event_cb_t cb = &fake_event_cb;

  const int id = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);

  const event_t* e = list_first_entry(&s.events, event_t, next);

  assert_true(TV_IS_INF(e->timeout));

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_set_timeout_invalid_event(void **state)
{
  // Invalid event ID will return EINVAL
  scheduler_t sched;
  event_id_t event_id = 0;
  struct timeval timeo;
  const int r = scheduler_event_set_timeout(&sched, event_id, timeo);
  assert_int_equal(r, -EINVAL);
}

void
test_scheduler_set_timeout_on_non_polled_event(void **state)
{
  // Set timeout on none polled event returns EINVAL
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_READ_FD; // Not a POLL_TIMEOUT event
  const int fd = mock_fd_create();
  struct timeval timeout = { .tv_sec = 996 };
  event_cb_t cb = &fake_event_cb;

  const int id = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const event_t* e = list_first_entry(&s.events, event_t, next);
  const int r = scheduler_event_set_timeout(&s, e->id, (struct timeval){});
  assert_int_equal(r, -EINVAL);

  close(fd);
  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_set_timeout_missing_event(void **state)
{
  // Set timeout returns ENOENT if event is not in list
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  struct timeval timeout = { .tv_sec = 996 };
  event_cb_t cb = &fake_event_cb;

  const int id = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const event_t* e = list_first_entry(&s.events, event_t, next);
  const int r = scheduler_event_set_timeout(&s, e->id+1, (struct timeval){});
  assert_int_equal(r, -ENOENT);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_set_timeout(void **state)
{
  // Set timeout will update timeout and deadline for affected event
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  struct timeval timeout1 = { .tv_sec = 996 };
  struct timeval timeout2 = { .tv_sec = 993 };
  struct timeval timeout3 = { .tv_sec = 964 };
  event_cb_t cb = &fake_event_cb;

  int fake_gettimeofday_tv_sec = 1;
  fake_gettimeofday = (struct timeval){ .tv_sec = fake_gettimeofday_tv_sec, .tv_usec = 0};

  const int id1 = scheduler_register_event(&s, mode, fd, timeout1, cb, NULL);
  const int id2 = scheduler_register_event(&s, mode, fd, timeout2, cb, NULL);
  const int id3 = scheduler_register_event(&s, mode, fd, timeout3, cb, NULL);
  const event_t* e1 = list_first_entry(&s.events, event_t, next);
  const event_t* e2 = list_next_entry(e1, next);
  const event_t* e3 = list_next_entry(e2, next);

  assert_int_equal(e1->timeout.tv_sec, timeout1.tv_sec);
  assert_int_equal(e2->timeout.tv_sec, timeout2.tv_sec);
  assert_int_equal(e3->timeout.tv_sec, timeout3.tv_sec);
  assert_int_equal(e1->deadline.tv_sec, fake_gettimeofday_tv_sec + timeout1.tv_sec);
  assert_int_equal(e2->deadline.tv_sec, fake_gettimeofday_tv_sec + timeout2.tv_sec);
  assert_int_equal(e3->deadline.tv_sec, fake_gettimeofday_tv_sec + timeout3.tv_sec);

  struct timeval new_timeout2 = { .tv_sec = 911 };
  scheduler_event_set_timeout(&s, e2->id, new_timeout2);

  assert_int_equal(e1->timeout.tv_sec, timeout1.tv_sec);     // unchanged
  assert_int_equal(e2->timeout.tv_sec, new_timeout2.tv_sec); // new value
  assert_int_equal(e3->timeout.tv_sec, timeout3.tv_sec);     // unchanged
  assert_int_equal(e1->deadline.tv_sec, fake_gettimeofday_tv_sec + timeout1.tv_sec);
  assert_int_equal(e2->deadline.tv_sec, fake_gettimeofday_tv_sec + new_timeout2.tv_sec);
  assert_int_equal(e3->deadline.tv_sec, fake_gettimeofday_tv_sec + timeout3.tv_sec);

  scheduler_unregister_event(&s, id1);
  scheduler_unregister_event(&s, id2);
  scheduler_unregister_event(&s, id3);
  scheduler_gc_events(&s);
}

void
test_scheduler_set_timeout_inf_and_deadline(void **state)
{
  // Set timeout will update deadline to TV_INF if new timeout is TV_INF
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  struct timeval timeout = { .tv_sec = 996 };
  event_cb_t cb = &fake_event_cb;

  fake_gettimeofday = (struct timeval){ .tv_sec = 1, .tv_usec = 2};

  const int id = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const event_t* e = list_first_entry(&s.events, event_t, next);

  assert_int_equal(e->timeout.tv_sec, timeout.tv_sec);

  struct timeval new_timeout = TV_INF;
  scheduler_event_set_timeout(&s, e->id, new_timeout);

  assert_true(TV_IS_INF(e->timeout));
  assert_true(TV_IS_INF(e->deadline));

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_unregister_event_will_set_dead_field(void **state)
{
  // Unregister event will set dead to 1
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  struct timeval timeout = { .tv_sec = 1 };
  event_cb_t cb = &fake_event_cb;

  const int id = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const event_t* e = list_first_entry(&s.events, event_t, next);
  assert_int_not_equal(e->dead, 1);

  scheduler_unregister_event(&s, e->id);
  assert_int_equal(e->dead, 1);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_unregister_event_will_ignore_invalid_event(void **state)
{
  // Unregister event will ignore invalid event id
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  struct timeval timeout = { .tv_sec = 1 };
  event_cb_t cb = &fake_event_cb;

  const int id = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const event_t* e = list_first_entry(&s.events, event_t, next);
  assert_int_not_equal(e->dead, 1);

  scheduler_unregister_event(&s, 0);
  assert_int_not_equal(e->dead, 1);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_mask_event_will_set_masked_field(void **state)
{
  // mask event will set masked to 1 or 0 depending on 3rd arg
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  struct timeval timeout = { .tv_sec = 1 };
  event_cb_t cb = &fake_event_cb;

  const int id = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const event_t* e = list_first_entry(&s.events, event_t, next);
  assert_int_not_equal(e->masked, 1);

  scheduler_mask_event(&s, e->id, 1);
  assert_int_equal(e->masked, 1);

  scheduler_mask_event(&s, e->id, 0);
  assert_int_not_equal(e->masked, 1);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_mask_event_will_accept_non_zero_value(void **state)
{
  // mask event will accept any non-zero value and convert it to 1
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  struct timeval timeout = { .tv_sec = 1 };
  event_cb_t cb = &fake_event_cb;

  const int id = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const event_t* e = list_first_entry(&s.events, event_t, next);
  assert_int_not_equal(e->masked, 1);

  scheduler_mask_event(&s, e->id, 959);
  assert_int_equal(e->masked, 1);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_mask_event_will_ignore_invalid_event_id(void **state)
{
  // mask event will ignore invalid event id
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  struct timeval timeout = { .tv_sec = 1 };
  event_cb_t cb = &fake_event_cb;

  const int id = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const event_t* e = list_first_entry(&s.events, event_t, next);
  assert_int_not_equal(e->masked, 1);

  scheduler_mask_event(&s, 0, 1);
  assert_int_not_equal(e->masked, 1);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_get_uuid(void **state)
{
  scheduler_t s;
  scheduler_initialize(&s);

  s.uuid = 1;
  const event_id_t new_uuid = scheduler_get_event_uuid(&s);
  assert_int_equal(new_uuid, 1);
  assert_int_equal(s.uuid, 2);
}

void
test_scheduler_get_uuid_overflow(void **state)
{
  // get uuid overflow
  scheduler_t s;
  scheduler_initialize(&s);

  s.uuid = INT_MAX;
  const event_id_t new_uuid = scheduler_get_event_uuid(&s);
  assert_int_equal(new_uuid, INT_MAX);
  assert_int_equal(s.uuid, 1);

  const event_id_t new_uuid2 = scheduler_get_event_uuid(&s);
  assert_int_equal(new_uuid2, 1);
  assert_int_equal(s.uuid, 2);
}

void
test_scheduler_get_uuid_overflow_fragmented(void **state)
{
  // get uuid overflow fragmented
  scheduler_t s;
  scheduler_initialize(&s);

  // Create a fragmented event list
  // +---+---+---+---
  // | 1 | 3 |   |...
  // +---+---+---+---
  s.uuid = 1;
  const int id1 = scheduler_register_event(&s, SCHEDULER_POLL_TIMEOUT, 1,
                                 (struct timeval){}, &fake_event_cb, NULL);

  s.uuid = 3;
  const int id2 = scheduler_register_event(&s, SCHEDULER_POLL_TIMEOUT, 1,
                                 (struct timeval){}, &fake_event_cb, NULL);

  // After an overflow the next UUID should be 2
  // because that is the next free event id
  s.uuid = INT_MIN;
  const event_id_t new_uuid1 = scheduler_get_event_uuid(&s);
  assert_int_equal(new_uuid1, 2);
  // +---+---+---+---
  // | 1 | 3 | 2 |...
  // +---+---+---+---

  // The next UUID after that will be 4 because 3 is already used.
  const event_id_t new_uuid2 = scheduler_get_event_uuid(&s);
  assert_int_equal(new_uuid2, 4);
  // +---+---+---+---+---
  // | 1 | 3 | 2 | 4 |...
  // +---+---+---+---+---

  scheduler_unregister_event(&s, id1);
  scheduler_unregister_event(&s, id2);
  scheduler_gc_events(&s);
}

void
test_scheduler_gc_will_remove_dead_events_from_list(void **state)
{
  // gc will remove dead events from list
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  struct timeval timeout = { .tv_sec = 1 };
  event_cb_t cb = &fake_event_cb;

  // Create a 3 event list
  // +---+  +---+  +---+
  // | 1 |->| 2 |->| 3 |
  // +---+  +---+  +---+
  const int id1 = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const int id2 = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const int id3 = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const event_t* e1 = list_first_entry(&s.events, event_t, next);
  const event_t* e2 = list_next_entry(e1, next);
  const event_t* e3 = list_next_entry(e2, next);

  // Unregister event 2
  // +---+  +----+  +---+
  // | 1 |->|dead|->| 3 |
  // +---+  +----+  +---+
  assert_int_equal(event_queue_length(&s), 3);
  scheduler_unregister_event(&s, e2->id);
  assert_int_equal(event_queue_length(&s), 3);

  // Call the GC to delete dead events
  // +---+  +---+
  // | 1 |->| 3 |
  // +---+  +---+
  scheduler_gc_events(&s);
  assert_int_equal(event_queue_length(&s), 2);

  // event 1 is now linked to event 3
  assert_ptr_equal(list_next_entry(e1, next), e3);

  scheduler_unregister_event(&s, id1);
  scheduler_unregister_event(&s, id2);
  scheduler_unregister_event(&s, id3);
  scheduler_gc_events(&s);
}

void
test_scheduler_check_timeouts(void **state)
{
  // scheduler_check_timeouts
  scheduler_t s;
  scheduler_initialize(&s);

  char mode = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  struct timeval timeout = {};
  event_cb_t cb = &fake_event_cb;

  const int id1 = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const int id2 = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const int id3 = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const int id4 = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const int id5 = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);
  const int id6 = scheduler_register_event(&s, mode, fd, timeout, cb, NULL);

  event_t* e1 = list_first_entry(&s.events, event_t, next);
  event_t* e2 = list_next_entry(e1, next);
  event_t* e3 = list_next_entry(e2, next);
  event_t* e4 = list_next_entry(e3, next);
  event_t* e5 = list_next_entry(e4, next);
  event_t* e6 = list_next_entry(e5, next);

  // Set current time to 2
  fake_gettimeofday = (struct timeval){ .tv_sec = 2};

  e1->dead = true;                      // 1: skip because dead
  e2->pending = SCHEDULER_POLL_TIMEOUT; // 2: skip because already pending
  e3->mode = SCHEDULER_POLL_READ_FD;    // 3: skip because TIMEOUT mode
  e4->timeout = TV_INF;                 // 4: skip because timeout is INF
  e5->deadline.tv_sec = 3;              // 5: skip because timeout not reached
  e6->deadline.tv_sec = 1;              // 6: mark because timeout has passed

  scheduler_check_timeouts(&s);
  assert_int_not_equal(e1->pending, SCHEDULER_POLL_TIMEOUT); // unchanged
  assert_int_equal(e2->pending, SCHEDULER_POLL_TIMEOUT);     // unchanged
  assert_int_not_equal(e3->pending, SCHEDULER_POLL_TIMEOUT); // unchanged
  assert_int_not_equal(e4->pending, SCHEDULER_POLL_TIMEOUT); // unchanged
  assert_int_not_equal(e5->pending, SCHEDULER_POLL_TIMEOUT); // unchanged
  assert_int_equal(e6->pending, SCHEDULER_POLL_TIMEOUT); // changed

  scheduler_unregister_event(&s, id1);
  scheduler_unregister_event(&s, id2);
  scheduler_unregister_event(&s, id3);
  scheduler_unregister_event(&s, id4);
  scheduler_unregister_event(&s, id5);
  scheduler_unregister_event(&s, id6);
  scheduler_gc_events(&s);
}

void
test_scheduler_callback(void **state)
{
  const int time_now1 = 911;
  const int time_now2 = 930;

  scheduler_t s;
  scheduler_initialize(&s);

  char md = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  const struct timeval to = { .tv_sec = 964 };
  event_cb_spy_t event_cb_spy = {};

  // Update current time to time_now1
  fake_gettimeofday = (struct timeval){ .tv_sec = time_now1 };

  const int id = scheduler_register_event(&s, md, fd, to, &mock_event_cb, &event_cb_spy);
  event_t* event1 = list_first_entry(&s.events, event_t, next);

  // Event deadline is using time_now1
  assert_int_equal(event1->deadline.tv_sec, to.tv_sec + time_now1);

  const char test_mode = 9;

  // Check callback has not been called
  assert_int_equal(event_cb_spy.was_called, 0);
  assert_int_not_equal(event_cb_spy.mode, test_mode);
  assert_int_not_equal(event_cb_spy.id, event1->id);

  // Update current time to time_now2
  fake_gettimeofday = (struct timeval){ .tv_sec = time_now2 };

  scheduler_event_callback(event1, test_mode);

  // Check callback has been called
  assert_int_equal(event_cb_spy.was_called, 1);
  assert_int_equal(event_cb_spy.mode, test_mode);
  assert_int_equal(event_cb_spy.id, event1->id);

  // Event deadline has been updated using time_now2
  assert_int_equal(event1->deadline.tv_sec, to.tv_sec + time_now2);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_callback_ignores_masked_events(void **state)
{
  // callback ignores masked events
  scheduler_t s;
  scheduler_initialize(&s);

  char md = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  const struct timeval to = {};
  event_cb_spy_t event_cb_spy = {};

  const int id = scheduler_register_event(&s, md, fd, to, &mock_event_cb, &event_cb_spy);
  event_t* event1 = list_first_entry(&s.events, event_t, next);

  // Mask the event here
  event1->masked = true;

  const int test_mode = 1;
  scheduler_event_callback(event1, test_mode);

  // Check callback has not been called
  assert_int_equal(event_cb_spy.was_called, 0);
  assert_int_not_equal(event_cb_spy.mode, test_mode);
  assert_int_not_equal(event_cb_spy.id, event1->id);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_run_events_run_callback_if_pending(void **state)
{
  // scheduler_run_events will run callback if event pending
  scheduler_t s;
  scheduler_initialize(&s);

  char md = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  const struct timeval to = {};
  event_cb_spy_t event_cb_spy = {};

  const int id = scheduler_register_event(&s, md, fd, to, &mock_event_cb, &event_cb_spy);
  event_t* event = list_first_entry(&s.events, event_t, next);

  // Set event to pending
  event->pending = SCHEDULER_POLL_TIMEOUT;

  const int n_dispatched = scheduler_run_events(&s);

  assert_int_equal(n_dispatched, 1);
  assert_int_equal(event_cb_spy.was_called, 1);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_run_events_no_callback_if_not_pending(void **state)
{
  // scheduler_run_events will ignore callback if event not pending
  scheduler_t s;
  scheduler_initialize(&s);

  char md = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  const struct timeval to = {};
  event_cb_spy_t event_cb_spy = {};

  const int id = scheduler_register_event(&s, md, fd, to, &mock_event_cb, &event_cb_spy);
  event_t* event = list_first_entry(&s.events, event_t, next);

  event->pending = 0;

  const int n_dispatched = scheduler_run_events(&s);

  assert_int_equal(n_dispatched, 0);
  assert_int_equal(event_cb_spy.was_called, 0);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_run_events_pending_mode_is_reset(void **state)
{
  // scheduler_run_events pending mode is reset on the event but still passed into callback
  scheduler_t s;
  scheduler_initialize(&s);

  char md = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  const struct timeval to = {};
  event_cb_spy_t event_cb_spy = {};

  const int id = scheduler_register_event(&s, md, fd, to, &mock_event_cb, &event_cb_spy);
  event_t* event = list_first_entry(&s.events, event_t, next);

  // Set event to pending
  event->pending = SCHEDULER_POLL_TIMEOUT;

  (void)scheduler_run_events(&s);

  // The callback gets the original pending value
  assert_int_equal(event_cb_spy.mode, SCHEDULER_POLL_TIMEOUT);

  // Event pending flag is reset
  assert_int_not_equal(event->pending, SCHEDULER_POLL_TIMEOUT);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_run_events_ignore_event_if_dead(void **state)
{
  // scheduler_run_events will ignore callback if event is dead
  scheduler_t s;
  scheduler_initialize(&s);

  char md = SCHEDULER_POLL_TIMEOUT;
  int fd = 1;
  const struct timeval to = {};
  event_cb_spy_t event_cb_spy = {};

  const int id = scheduler_register_event(&s, md, fd, to, &mock_event_cb, &event_cb_spy);
  event_t* event = list_first_entry(&s.events, event_t, next);

  // Set event to pending
  event->pending = SCHEDULER_POLL_TIMEOUT;

  event->dead = true;

  const int n_dispatched = scheduler_run_events(&s);

  assert_int_equal(n_dispatched, 0);
  assert_int_equal(event_cb_spy.was_called, 0);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_run_events_no_events(void **state)
{
  // scheduler_run_events no events no problem
  scheduler_t s;
  scheduler_initialize(&s);

  const int n_dispatched = scheduler_run_events(&s);

  assert_int_equal(n_dispatched, 0);
}

void
test_scheduler_prepare_events_no_events(void **state)
{
  // scheduler_prepare_events no events no problem
  scheduler_t s;
  scheduler_initialize(&s);
  scheduler_prepare_events(&s);
  assert_int_equal(s.max_fd, -1);
}

void
test_scheduler_prepare_events_masked_event_ignored(void **state)
{
  // scheduler_prepare_events masked event is ignored
  scheduler_t s;
  scheduler_initialize(&s);

  const char md = SCHEDULER_POLL_TIMEOUT;
  const int fd = 1;
  const struct timeval to = {};
  const int id = scheduler_register_event(&s, md, fd, to, &fake_event_cb, NULL);
  event_t* event = list_first_entry(&s.events, event_t, next);

  // Mask the event here
  event->masked = true;

  scheduler_prepare_events(&s);

  assert_int_equal(s.max_fd, -1);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_prepare_events_dead_event_ignored(void **state)
{
  // scheduler_prepare_events dead event is ignored
  scheduler_t s;
  scheduler_initialize(&s);

  const char md = SCHEDULER_POLL_TIMEOUT;
  const int fd = 1;
  const struct timeval to = {};
  const int id = scheduler_register_event(&s, md, fd, to, &fake_event_cb, NULL);
  event_t* event = list_first_entry(&s.events, event_t, next);

  // Unalive event here
  event->dead = true;

  scheduler_prepare_events(&s);

  assert_int_equal(s.max_fd, -1);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_add_read_event(void **state)
{
  // scheduler_prepare_events add READ_FD
  scheduler_t s;
  scheduler_initialize(&s);

  const char md = SCHEDULER_POLL_READ_FD;
  const int test_fd = 991;
  const struct timeval to = {};
  const int id = scheduler_register_event(&s, md, test_fd, to, &fake_event_cb, NULL);

  scheduler_prepare_events(&s);

  assert_int_equal(s.max_fd, test_fd);

  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_read_event_with_invalid_fd(void **state)
{
  // scheduler_prepare_events READ_FD event with invalid file descriptor is ignored
  scheduler_t s;
  scheduler_initialize(&s);

  const char md = SCHEDULER_POLL_READ_FD;
  const int fd = mock_fd_create();
  const struct timeval to = {};
  const int event_id = scheduler_register_event(&s, md, fd, to, &fake_event_cb, NULL);
  event_t* event = list_first_entry(&s.events, event_t, next);

  // Invalid event
  event->fd = -2;

  scheduler_prepare_events(&s);

  assert_int_equal(s.max_fd, -1);

  close(fd);
  scheduler_unregister_event(&s, event_id);
  scheduler_gc_events(&s);
}

void
test_scheduler_add_write_event(void **state)
{
  // scheduler_prepare_events add WRITE_FD
  scheduler_t s;
  scheduler_initialize(&s);

  const char md = SCHEDULER_POLL_WRITE_FD;
  const int test_fd = 991;
  const struct timeval to = {};
  const int event_id = scheduler_register_event(&s, md, test_fd, to, &fake_event_cb, NULL);

  scheduler_prepare_events(&s);

  assert_int_equal(s.max_fd, test_fd);

  scheduler_unregister_event(&s, event_id);
  scheduler_gc_events(&s);
}

void
test_scheduler_write_event_with_invalid_fd(void **state)
{
  // scheduler_prepare_events WRITE_FD event with invalid file descriptor is ignored
  scheduler_t s;
  scheduler_initialize(&s);

  const char md = SCHEDULER_POLL_WRITE_FD;
  const int fd = mock_fd_create();
  const struct timeval to = {};
  const int event_id = scheduler_register_event(&s, md, fd, to, &fake_event_cb, NULL);
  event_t* event = list_first_entry(&s.events, event_t, next);

  // Invalid event
  event->fd = -2;

  scheduler_prepare_events(&s);

  assert_int_equal(s.max_fd, -1);

  close(fd);
  scheduler_unregister_event(&s, event_id);
  scheduler_gc_events(&s);
}

void
test_scheduler_add_except_event(void **state)
{
  // scheduler_prepare_events add EXCEPT_FD
  scheduler_t s;
  scheduler_initialize(&s);

  const char md = SCHEDULER_POLL_EXCEPT_FD;
  const int test_fd = 991;
  const struct timeval to = {};
  const int event_id = scheduler_register_event(&s, md, test_fd, to, &fake_event_cb, NULL);

  scheduler_prepare_events(&s);

  assert_int_equal(s.max_fd, test_fd);

  scheduler_unregister_event(&s, event_id);
  scheduler_gc_events(&s);
}

void
test_scheduler_except_event_with_invalid_fd(void **state)
{
  // scheduler_prepare_events EXCEPT_FD event with invalid file descriptor is ignored
  scheduler_t s;
  scheduler_initialize(&s);

  const char md = SCHEDULER_POLL_EXCEPT_FD;
  const int fd = mock_fd_create();
  const struct timeval to = {};
  const int event_id = scheduler_register_event(&s, md, fd, to, &fake_event_cb, NULL);
  event_t* event = list_first_entry(&s.events, event_t, next);

  // Invalid event
  event->fd = -2;

  scheduler_prepare_events(&s);

  assert_int_equal(s.max_fd, -1);
  close(fd);
  scheduler_unregister_event(&s, event_id);
  scheduler_gc_events(&s);
}

void
test_scheduler_no_timeout_events_then_timeout_is_max(void **state)
{
  // scheduler_prepare_events with no TIMEOUT events the timeout is MAX
  scheduler_t s;
  scheduler_initialize(&s);
  s.max_timeout = TV_SECS(600); // FIXME

  const char md = SCHEDULER_POLL_EXCEPT_FD;
  const int test_fd = mock_fd_create();
  const struct timeval to = {};
  const int event_id = scheduler_register_event(&s, md, test_fd, to, &fake_event_cb, NULL);

  scheduler_prepare_events(&s);

  const struct timeval expected_tv = TV_SECS(600);
  assert_int_equal(s.timeout.tv_sec, expected_tv.tv_sec);

  close(test_fd);
  scheduler_unregister_event(&s, event_id);
  scheduler_gc_events(&s);
}

void
test_scheduler_add_timeout_event(void **state)
{
  // scheduler_prepare_events add TIMEOUT event
  scheduler_t s;
  scheduler_initialize(&s);
  s.max_timeout = TV_SECS(600);

  const char md = SCHEDULER_POLL_TIMEOUT;
  const int fd = 1;
  const struct timeval to = { .tv_sec = 10 };
  fake_gettimeofday = (struct timeval){ .tv_sec = 0, .tv_usec = 0};
  const int event_id = scheduler_register_event(&s, md, fd, to, &fake_event_cb, NULL);

  const struct timeval time_now = { .tv_sec = 2, .tv_usec = 0};
  fake_gettimeofday = time_now;

  scheduler_prepare_events(&s);

  // New timeout value is time to the event deadline
  assert_int_equal(s.timeout.tv_sec, to.tv_sec - time_now.tv_sec);

  scheduler_unregister_event(&s, event_id);
  scheduler_gc_events(&s);
}

void
test_scheduler_multiple_timeout_events_use_lowest_timeout(void **state)
{
  // scheduler_prepare_events add multiple TIMEOUT events new timeout is lowest
  scheduler_t s;
  scheduler_initialize(&s);
  s.max_timeout = TV_SECS(600);

  const char md = SCHEDULER_POLL_TIMEOUT;
  const int fd = 1;
  const struct timeval to1 = { .tv_sec = 20 };
  const struct timeval to2 = { .tv_sec = 10 };
  fake_gettimeofday = (struct timeval){ .tv_sec = 0, .tv_usec = 0};
  const int id1 = scheduler_register_event(&s, md, fd, to1, &fake_event_cb, NULL);
  const int id2 = scheduler_register_event(&s, md, fd, to2, &fake_event_cb, NULL);

  const struct timeval time_now = { .tv_sec = 2, .tv_usec = 0};
  fake_gettimeofday = time_now;

  scheduler_prepare_events(&s);

  // New timeout is based on the smaller event timeout value (to2).
  assert_int_equal(s.timeout.tv_sec, to2.tv_sec - time_now.tv_sec);

  scheduler_unregister_event(&s, id1);
  scheduler_unregister_event(&s, id2);
  scheduler_gc_events(&s);
}

void
test_scheduler_timeout_event_is_instant_if_deadline_is_now(void **state)
{
  // scheduler_prepare_events add TIMEOUT event timeout is zero if deadline is now
  scheduler_t s;
  scheduler_initialize(&s);
  s.max_timeout = TV_SECS(600);

  const char md = SCHEDULER_POLL_TIMEOUT;
  const int fd = 1;
  const struct timeval to = { .tv_sec = 10 };
  fake_gettimeofday = (struct timeval){ .tv_sec = 0, .tv_usec = 0};
  const int event_id = scheduler_register_event(&s, md, fd, to, &fake_event_cb, NULL);

  // Set the time now to the event timeout
  fake_gettimeofday = to;

  scheduler_prepare_events(&s);

  // New timeout is zero because deadline has already been reached.
  assert_int_equal(s.timeout.tv_sec, 0);

  scheduler_unregister_event(&s, event_id);
  scheduler_gc_events(&s);
}

void
test_scheduler_multiple_timeout_events_dont_interfere(void **state)
{
  // scheduler_prepare_events multiple timeout events don't clobber each other
  scheduler_t s;
  scheduler_initialize(&s);
  s.max_timeout = TV_SECS(600);

  const char md = SCHEDULER_POLL_TIMEOUT;
  const int fd = 1;
  const struct timeval to1 = { .tv_sec = 10 };
  const struct timeval to2 = { .tv_sec = 20 };
  fake_gettimeofday = (struct timeval){ .tv_sec = 0, .tv_usec = 0};
  const int id1 = scheduler_register_event(&s, md, fd, to1, &fake_event_cb, NULL);
  const int id2 = scheduler_register_event(&s, md, fd, to2, &fake_event_cb, NULL);

  // Set the time now to the first event timeout
  fake_gettimeofday = to1;

  scheduler_prepare_events(&s);

  // Event though event2 still has 10 seconds left event1 is 0 therefor timeout is 0
  assert_int_equal(s.timeout.tv_sec, 0);

  scheduler_unregister_event(&s, id1);
  scheduler_unregister_event(&s, id2);
  scheduler_gc_events(&s);
}

void
test_scheduler_timeout_event_ignored_if_no_timeout(void **state)
{
  // scheduler_prepare_events add TIMEOUT event ignored if no timeout
  scheduler_t s;
  scheduler_initialize(&s);
  s.max_timeout = TV_SECS(600);

  const char md = SCHEDULER_POLL_TIMEOUT;
  const int fd = 1;
  const struct timeval to = TV_INF;
  fake_gettimeofday = (struct timeval){ .tv_sec = 0, .tv_usec = 0};
  const int id = scheduler_register_event(&s, md, fd, to, &fake_event_cb, NULL);

  scheduler_prepare_events(&s);

  assert_int_equal(s.max_fd, -1);
  scheduler_unregister_event(&s, id);
  scheduler_gc_events(&s);
}

void
test_scheduler_with_no_events_will_timeout(void **state)
{
  // Scheduler with no events will timeout
  scheduler_t s;
  scheduler_initialize(&s);

  const int ret = scheduler_wait_for_events(&s);
  assert_int_equal(ret, 0);
}

/*
 * Test running a single SCHEDULER_POLL_READ_FD event to completion.
 */
void
test_scheduler_run_single_read_fd(void **state)
{
  scheduler_t s;
  scheduler_initialize(&s);

  const int fd = mock_fd_create();

  /* Create a scheduler event for this fd.
   * The callback will be called when fd is ready for reading. */
  event_cb_spy_t event_cb_spy = {};
  int event_id;
  {
    const char mode = SCHEDULER_POLL_READ_FD;
    const struct timeval timeout = {};
    event_id = scheduler_register_event(&s, mode, fd, timeout, &mock_event_cb, &event_cb_spy);
    assert_int_not_equal(event_id, 0);
  }

  /* Tick 1 - nothing changed so should timeout with no callback */
  assert_int_equal(scheduler_wait_for_events(&s), 0);
  assert_int_equal(event_cb_spy.was_called, 0);

  mock_fd_set_readable(fd);

  /* Tick 2 - fd has data, event callback should be called */
  assert_int_equal(scheduler_wait_for_events(&s), 0);
  assert_int_equal(event_cb_spy.was_called, 1);

  close(fd);
  scheduler_unregister_event(&s, event_id);
  scheduler_gc_events(&s);
}

/*
 * Test running a single SCHEDULER_POLL_WRITE_FD event to completion.
 * 1. Create a mock fd
 * 2. Create a SCHEDULER_POLL_WRITE_FD event for pipe
 * 3. Manually tick the scheduler
 * 4. Check that the event callback fired
 * 5. Set the mock fd to unwriteable
 * 6. Manually tick the scheduler
 * 7. Check that the event callback did not fire
 */
void
test_scheduler_run_single_write_fd(void **state)
{
  scheduler_t s;
  scheduler_initialize(&s);

  const int fd = mock_fd_create();

  /* Create a scheduler event for this fd.
   * The callback will be called when fd is ready for writing. */
  event_cb_spy_t event_cb_spy = {};
  int event_id;
  {
    const char mode = SCHEDULER_POLL_WRITE_FD;
    const struct timeval timeout = {};
    event_id = scheduler_register_event(&s, mode, fd, timeout, &mock_event_cb, &event_cb_spy);
    assert_int_not_equal(event_id, 0);
  }

  /* Tick 1 - fd is ready for writing so event callback should be called */
  assert_int_equal(scheduler_wait_for_events(&s), 0);
  assert_int_equal(event_cb_spy.was_called, 1);

  mock_fd_set_unwritable(fd);

  /* We expect a timeout next tick so set a low value */
  scheduler_set_max_timeout(&s, (struct timeval){ .tv_sec = 0, .tv_usec = 500 });

  /* Tick 2 - fd is not ready for writing so should timeout and callback is not called*/
  assert_int_equal(scheduler_wait_for_events(&s), 0);
  assert_int_equal(event_cb_spy.was_called, 1);

  mock_fd_set_writable(fd);

  /* Tick 3 - fd is ready again so callback should be called */
  assert_int_equal(scheduler_wait_for_events(&s), 0);
  assert_int_equal(event_cb_spy.was_called, 2);

  close(fd);
  scheduler_unregister_event(&s, event_id);
  scheduler_gc_events(&s);
}

#if 0
void
test_scheduler_run_single_except_fd(void **state)
{
  No code uses SCHEDULER_POLL_EXCEPT_FD and it is not trivial to test.

  According to man pages these are what count as exceptional conditions:
  - There is out-of-band data on a TCP socket (see tcp(7)).
  - A pseudoterminal master in packet mode has seen a state change on the slave
  - A cgroup.events file has been modified (see cgroups(7)).

  None of which seem worth testing for here.
}
#endif

/*
 * Test running a single SCHEDULER_POLL_READ_FD event but then cancelling it.
 * 1. Create a mock fd
 * 2. Create a SCHEDULER_POLL_READ_FD event for pipe
 * 3. Make the mock fd readable
 * 4. Unreregister the event
 * 5. Manually tick the scheduler
 * 6. Check that the event callback did not fire
 */
void
test_scheduler_run_single_dead_event(void **state)
{
  scheduler_t s;
  scheduler_initialize(&s);

  const int fd = mock_fd_create();

  /* Create a scheduler event for this fd.
   * The callback will be called when fd is ready for reading. */
  event_cb_spy_t event_cb_spy = {};
  int event_id = -1;
  {
    const char mode = SCHEDULER_POLL_READ_FD;
    const struct timeval timeout = {};
    event_id = scheduler_register_event(&s, mode, fd, timeout, &mock_event_cb, &event_cb_spy);
    assert_true(event_id > 0);
  }

  /* Tick 1 - nothing changed so should timeout with no callback */
  assert_int_equal(scheduler_wait_for_events(&s), 0);
  assert_int_equal(event_cb_spy.was_called, 0);

  mock_fd_set_readable(fd);

  const event_t* e1 = list_first_entry(&s.events, event_t, next);
  scheduler_unregister_event(&s, event_id);
  /* Event struct still exists but is marked as dead */
  assert_int_equal(e1->id, event_id);
  assert_int_equal(e1->dead, 1);

  /* We expect a timeout next tick so set a low value */
  scheduler_set_max_timeout(&s, (struct timeval){ .tv_sec = 0, .tv_usec = 500 });

  /* Tick 2 - fd has data, but event was unregistered so expect no callback */
  assert_int_equal(scheduler_wait_for_events(&s), 0);
  assert_int_equal(event_cb_spy.was_called, 0);

  /* The garbage collector has now removed the dead event from the list */
  const event_t* e2 = list_first_entry(&s.events, event_t, next);
  /* The head of the list is now an empty placeholder. */
  assert_int_equal(e2->id, 0);
  assert_ptr_not_equal(e2, e1); /* The dead event is gone. */

  close(fd);
  scheduler_unregister_event(&s, event_id);
  scheduler_gc_events(&s);
}

/* Create two events with the same fd but different callbacks.
 * Only the first registered callback should fire.
 * If we unregister the first event then the second callback should fire next.
 *
 * This seems to be more of an accident of how the earlier select version of
 * the scheduler was written rather than a concious design decision.
 *
 * It is important to preserve this behaviour because some parts of blktap will
 * accidentally register multiple events with the same fd but then break if the
 * callback is called more than once.
 */
void
test_scheduler_run_duplicate_fds_are_handled_once(void **state)
{
  scheduler_t s;
  scheduler_initialize(&s);

  const int fd = mock_fd_create();

  event_cb_spy_t event_cb_spy1 = {};
  event_cb_spy_t event_cb_spy2 = {};

  const char mode = SCHEDULER_POLL_WRITE_FD;
  const struct timeval timeout = {};

  /* Create a scheduler event for this fd. */
  const int event_id1 = scheduler_register_event(&s, mode, fd, timeout, &mock_event_cb, &event_cb_spy1);
  assert_int_not_equal(event_id1, 0);

  /* Register same fd and callback again */
  const int event_id2 = scheduler_register_event(&s, mode, fd, timeout, &mock_event_cb, &event_cb_spy2);
  assert_int_not_equal(event_id2, 0);

  /* The scheduler regards them as two separate events */
  assert_int_not_equal(event_id1, event_id2);

  assert_int_equal(scheduler_wait_for_events(&s), 0);

  /* Only first callback should have fired */
  assert_int_equal(event_cb_spy1.was_called, 1);
  assert_int_equal(event_cb_spy2.was_called, 0);

  scheduler_unregister_event(&s, event_id1);

  assert_int_equal(scheduler_wait_for_events(&s), 0);

  /* Now the second callback should have fired instead */
  assert_int_equal(event_cb_spy1.was_called, 1);
  assert_int_equal(event_cb_spy2.was_called, 1);

  close(fd);
  scheduler_unregister_event(&s, event_id1);
  scheduler_unregister_event(&s, event_id2);
  scheduler_gc_events(&s);
}

/* Register two events with different fds but the same callback.
 * The callback should be called twice when both fds are ready.
 */
void
test_scheduler_run_with_duplicate_callbacks(void **state)
{
  scheduler_t s;
  scheduler_initialize(&s);

  const int fd1 = mock_fd_create();
  const int fd2 = mock_fd_create();

  event_cb_spy_t event_cb_spy = {};

  int event_id1, event_id2;
  {
    const char mode = SCHEDULER_POLL_WRITE_FD;
    const struct timeval timeout = {};

    /* Create a scheduler event for fd1. */
    event_id1 = scheduler_register_event(&s, mode, fd1, timeout, &mock_event_cb, &event_cb_spy);
    assert_int_not_equal(event_id1, 0);

    /* Create a scheduler event for fd2 but same callback */
    event_id2 = scheduler_register_event(&s, mode, fd2, timeout, &mock_event_cb, &event_cb_spy);
    assert_int_not_equal(event_id2, 0);
  }

  /* Tick the scheduler */
  assert_int_equal(scheduler_wait_for_events(&s), 0);

  /* Callback is called twice because both events had a different fd */
  assert_int_equal(event_cb_spy.was_called, 2);

  close(fd1);
  close(fd2);
  scheduler_unregister_event(&s, event_id1);
  scheduler_unregister_event(&s, event_id2);
  scheduler_gc_events(&s);
}

void
test_scheduler_run_read_and_write_fd(void **state)
{
  scheduler_t s;
  scheduler_initialize(&s);

  const int fd = mock_fd_create();

  event_cb_spy_t event_cb_spy1 = {};
  event_cb_spy_t event_cb_spy2 = {};

  int event_id1, event_id2;
  {
    const char mode = SCHEDULER_POLL_WRITE_FD;
    const struct timeval timeout = {};

    /* Create a write event for fd. */
    event_id1 = scheduler_register_event(&s, mode, fd, timeout, &mock_event_cb, &event_cb_spy1);
    assert_int_not_equal(event_id1, 0);
  }

  {
    const char mode = SCHEDULER_POLL_READ_FD;
    const struct timeval timeout = {};

    /* Create a read event for fd1. */
    event_id2 = scheduler_register_event(&s, mode, fd, timeout, &mock_event_cb, &event_cb_spy2);
    assert_int_not_equal(event_id2, 0);
  }

  /* Make the fd read and writable */
  mock_fd_set_readable(fd);

  /* Tick the scheduler */
  assert_int_equal(scheduler_wait_for_events(&s), 0);

  /* Both read and write callbacks were called */
  assert_int_equal(event_cb_spy1.was_called, 1);
  assert_int_equal(event_cb_spy2.was_called, 1);

  /* Tick the scheduler again*/
  assert_int_equal(scheduler_wait_for_events(&s), 0);

  /* Both read and write callbacks were called again */
  assert_int_equal(event_cb_spy1.was_called, 2);
  assert_int_equal(event_cb_spy2.was_called, 2);

  /* Unregister the READ event */
  scheduler_unregister_event(&s, event_id2);

  assert_int_equal(scheduler_wait_for_events(&s), 0);

  /* Only the WRITE event callback is called */
  assert_int_equal(event_cb_spy1.was_called, 3);
  assert_int_equal(event_cb_spy2.was_called, 2);

  close(fd);
  scheduler_unregister_event(&s, event_id1);
  scheduler_unregister_event(&s, event_id2);
  scheduler_gc_events(&s);
}

void
test_scheduler_run_deleted_duplicate_event(void **state)
{
  scheduler_t s;
  scheduler_initialize(&s);

  const int fd = mock_fd_create();

  event_cb_spy_t event_cb_spy1 = {};
  event_cb_spy_t event_cb_spy2 = {};

  int event_id1, event_id2;
  {
    const char mode = SCHEDULER_POLL_WRITE_FD;
    const struct timeval timeout = {};

    /* Create two identical events with the same fd. */
    event_id1 = scheduler_register_event(&s, mode, fd, timeout, &mock_event_cb, &event_cb_spy1);
    assert_int_not_equal(event_id1, 0);

    event_id2 = scheduler_register_event(&s, mode, fd, timeout, &mock_event_cb, &event_cb_spy2);
    assert_int_not_equal(event_id2, 0);
  }

  /* Tick the scheduler */
  assert_int_equal(scheduler_wait_for_events(&s), 0);

  /* Only the callback registered first should fire */
  assert_int_equal(event_cb_spy1.was_called, 1);
  assert_int_equal(event_cb_spy2.was_called, 0);

  /* Unregister the first event */
  scheduler_unregister_event(&s, event_id1);

  /* Tick the scheduler again*/
  assert_int_equal(scheduler_wait_for_events(&s), 0);

  /* This time the second callback should fire */
  assert_int_equal(event_cb_spy1.was_called, 1);
  assert_int_equal(event_cb_spy2.was_called, 1);

  close(fd);

  scheduler_unregister_event(&s, event_id1);
  scheduler_unregister_event(&s, event_id2);
  scheduler_gc_events(&s);
}
