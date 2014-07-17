import unittest
import mock
import os
import gc

import testlib

import lock


class FailingOpenContext(testlib.TestContext):
    def fake_open(self, fname, mode='r'):
        raise IOError()


class TestLockDestruction(unittest.TestCase):
    def setUp(self):
        gc.disable()

    @testlib.with_custom_context(FailingOpenContext)
    def test_close_if_open_failed(self, ctx):
        try:
            lck = lock.Lock('somename')
            raise AssertionError('An IOError was expected here')
        except IOError:
            pass

        locks = self.retrieve_lock_instances_from_gc()
        self.assertEquals(1, len(locks))

        lck, = locks

        lck._close()

    def retrieve_lock_instances_from_gc(self):
        locks = []
        for obj in gc.get_objects():
            if isinstance(obj, lock.Lock):
                locks.append(obj)

        return locks

    def tearDown(self):
        gc.enable()
