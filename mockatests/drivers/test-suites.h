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

#ifndef __TEST_SUITES_H__
#define __TEST_SUITES_H__

#include <setjmp.h>
#include <cmocka.h>
#include <uuid/uuid.h>
#include <stdint.h>

void test_stats_normal_buffer(void **state);
void test_stats_realloc_buffer(void **state);
void test_stats_realloc_buffer_edgecase(void **state);

static const struct CMUnitTest tapdisk_stats_tests[] = {
	cmocka_unit_test(test_stats_normal_buffer),
	cmocka_unit_test(test_stats_realloc_buffer),
	cmocka_unit_test(test_stats_realloc_buffer_edgecase)
};

void test_vbd_linked_list(void **state);
void test_vbd_complete_td_request(void **state);
void test_vbd_issue_request(void **stat);
void test_vbd_complete_block_status_request(void **stat);

static const struct CMUnitTest tapdisk_vbd_tests[] = {
	cmocka_unit_test(test_vbd_linked_list),
	cmocka_unit_test(test_vbd_issue_request),
	cmocka_unit_test(test_vbd_complete_block_status_request)
};

void test_nbdserver_new_protocol_handshake(void **state);
void test_nbdserver_new_protocol_handshake_send_fails(void **state);
static const struct CMUnitTest tapdisk_nbdserver_tests[] = {
	cmocka_unit_test(test_nbdserver_new_protocol_handshake)
};

void test_scheduler_set_max_timeout(void **state);
void test_scheduler_set_max_timeout_lower(void **state);
void test_scheduler_set_max_timeout_higher(void **state);
void test_scheduler_set_max_timeout_negative(void **state);
void test_scheduler_set_max_timeout_inf(void **state);
void test_scheduler_register_event_null_callback(void **state);
void test_scheduler_register_event_bad_mode(void **state);
void test_scheduler_register_multiple_events(void **state);
void test_scheduler_register_event_populates_event(void **state);
void test_scheduler_set_timeout_inf(void **state);
void test_scheduler_set_timeout_invalid_event(void **state);
void test_scheduler_set_timeout_on_non_polled_event(void **state);
void test_scheduler_set_timeout_missing_event(void **state);
void test_scheduler_set_timeout(void **state);
void test_scheduler_set_timeout_inf_and_deadline(void **state);
void test_scheduler_unregister_event_will_set_dead_field(void **state);
void test_scheduler_unregister_event_will_ignore_invalid_event(void **state);
void test_scheduler_mask_event_will_set_masked_field(void **state);
void test_scheduler_mask_event_will_accept_non_zero_value(void **state);
void test_scheduler_mask_event_will_ignore_invalid_event_id(void **state);
void test_scheduler_get_uuid(void **state);
void test_scheduler_get_uuid_overflow(void **state);
void test_scheduler_get_uuid_overflow_fragmented(void **state);
void test_scheduler_gc_will_remove_dead_events_from_list(void **state);
void test_scheduler_check_timeouts(void **state);
void test_scheduler_callback(void **state);
void test_scheduler_callback_ignores_masked_events(void **state);
void test_scheduler_run_events_run_callback_if_pending(void **state);
void test_scheduler_run_events_no_callback_if_not_pending(void **state);
void test_scheduler_run_events_pending_mode_is_reset(void **state);
void test_scheduler_run_events_ignore_event_if_dead(void **state);
void test_scheduler_run_events_no_events(void **state);
void test_scheduler_prepare_events_no_events(void **state);
void test_scheduler_prepare_events_masked_event_ignored(void **state);
void test_scheduler_prepare_events_dead_event_ignored(void **state);
void test_scheduler_add_read_event(void **state);
void test_scheduler_read_event_with_invalid_fd(void **state);
void test_scheduler_add_write_event(void **state);
void test_scheduler_write_event_with_invalid_fd(void **state);
void test_scheduler_add_except_event(void **state);
void test_scheduler_except_event_with_invalid_fd(void **state);
void test_scheduler_no_timeout_events_then_timeout_is_max(void **state);
void test_scheduler_add_timeout_event(void **state);
void test_scheduler_multiple_timeout_events_use_lowest_timeout(void **state);
void test_scheduler_timeout_event_is_instant_if_deadline_is_now(void **state);
void test_scheduler_multiple_timeout_events_dont_interfere(void **state);
void test_scheduler_timeout_event_ignored_if_no_timeout(void **state);
void test_scheduler_with_no_events_will_timeout(void **state);
void test_scheduler_run_single_read_fd(void **state);
void test_scheduler_run_single_write_fd(void **state);
void test_scheduler_run_single_dead_event(void **state);

static const struct CMUnitTest tapdisk_sched_tests[] = {
  cmocka_unit_test(test_scheduler_set_max_timeout),
  cmocka_unit_test(test_scheduler_set_max_timeout_lower),
  cmocka_unit_test(test_scheduler_set_max_timeout_higher),
  cmocka_unit_test(test_scheduler_set_max_timeout_negative),
  cmocka_unit_test(test_scheduler_set_max_timeout_inf),
  cmocka_unit_test(test_scheduler_register_event_null_callback),
  cmocka_unit_test(test_scheduler_register_event_bad_mode),
  cmocka_unit_test(test_scheduler_register_multiple_events),
  cmocka_unit_test(test_scheduler_register_event_populates_event),
  cmocka_unit_test(test_scheduler_set_timeout_inf),
  cmocka_unit_test(test_scheduler_set_timeout_invalid_event),
  cmocka_unit_test(test_scheduler_set_timeout_on_non_polled_event),
  cmocka_unit_test(test_scheduler_set_timeout_missing_event),
  cmocka_unit_test(test_scheduler_set_timeout),
  cmocka_unit_test(test_scheduler_set_timeout_inf_and_deadline),
  cmocka_unit_test(test_scheduler_unregister_event_will_set_dead_field),
  cmocka_unit_test(test_scheduler_unregister_event_will_ignore_invalid_event),
  cmocka_unit_test(test_scheduler_mask_event_will_set_masked_field),
  cmocka_unit_test(test_scheduler_mask_event_will_accept_non_zero_value),
  cmocka_unit_test(test_scheduler_mask_event_will_ignore_invalid_event_id),
  cmocka_unit_test(test_scheduler_get_uuid),
  cmocka_unit_test(test_scheduler_get_uuid_overflow),
  cmocka_unit_test(test_scheduler_get_uuid_overflow_fragmented),
  cmocka_unit_test(test_scheduler_gc_will_remove_dead_events_from_list),
  cmocka_unit_test(test_scheduler_check_timeouts),
  cmocka_unit_test(test_scheduler_callback),
  cmocka_unit_test(test_scheduler_callback_ignores_masked_events),
  cmocka_unit_test(test_scheduler_run_events_run_callback_if_pending),
  cmocka_unit_test(test_scheduler_run_events_no_callback_if_not_pending),
  cmocka_unit_test(test_scheduler_run_events_pending_mode_is_reset),
  cmocka_unit_test(test_scheduler_run_events_ignore_event_if_dead),
  cmocka_unit_test(test_scheduler_run_events_no_events),
  cmocka_unit_test(test_scheduler_prepare_events_no_events),
  cmocka_unit_test(test_scheduler_prepare_events_masked_event_ignored),
  cmocka_unit_test(test_scheduler_prepare_events_dead_event_ignored),
  cmocka_unit_test(test_scheduler_add_read_event),
  cmocka_unit_test(test_scheduler_read_event_with_invalid_fd),
  cmocka_unit_test(test_scheduler_add_write_event),
  cmocka_unit_test(test_scheduler_write_event_with_invalid_fd),
  cmocka_unit_test(test_scheduler_add_except_event),
  cmocka_unit_test(test_scheduler_except_event_with_invalid_fd),
  cmocka_unit_test(test_scheduler_no_timeout_events_then_timeout_is_max),
  cmocka_unit_test(test_scheduler_add_timeout_event),
  cmocka_unit_test(test_scheduler_multiple_timeout_events_use_lowest_timeout),
  cmocka_unit_test(test_scheduler_timeout_event_is_instant_if_deadline_is_now),
  cmocka_unit_test(test_scheduler_multiple_timeout_events_dont_interfere),
  cmocka_unit_test(test_scheduler_timeout_event_ignored_if_no_timeout),
  cmocka_unit_test(test_scheduler_with_no_events_will_timeout),
  cmocka_unit_test(test_scheduler_run_single_read_fd),
  cmocka_unit_test(test_scheduler_run_single_write_fd),
  cmocka_unit_test(test_scheduler_run_single_dead_event),
};

#endif /* __TEST_SUITES_H__ */
