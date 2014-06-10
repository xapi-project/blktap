import unittest
import testlib
import lvmlib
import mock

import lvutil


ONE_MEGABYTE=1*1024*1024


def with_lvm_subsystem(func):
    @testlib.with_context
    def decorated(self, context, *args, **kwargs):
        lvsystem = lvmlib.LVSubsystem(context.log, context.add_executable)
        return func(self, lvsystem, *args, **kwargs)

    decorated.__name__ = func.__name__
    return decorated


class TestCreate(unittest.TestCase):
    @with_lvm_subsystem
    def test_create_volume_size(self, lvsystem):
        lvsystem.add_volume_group('vgroup')

        lvutil.create('volume', 100 * ONE_MEGABYTE, 'vgroup')

        created_lv, = lvsystem.get_logical_volumes_with_name('volume')

        self.assertEquals(100, created_lv.size_mb)

    @with_lvm_subsystem
    def test_create_volume_is_in_the_right_volume_group(self, lvsystem):
        lvsystem.add_volume_group('vgroup')

        lvutil.create('volume', 100 * ONE_MEGABYTE, 'vgroup')

        created_lv, = lvsystem.get_logical_volumes_with_name('volume')

        self.assertEquals(100, created_lv.size_mb)

        self.assertEquals('vgroup', created_lv.volume_group.name)
        self.assertTrue(created_lv.active)
        self.assertTrue(created_lv.zeroed)

    @with_lvm_subsystem
    def test_create_volume_is_active(self, lvsystem):
        lvsystem.add_volume_group('vgroup')

        lvutil.create('volume', 100 * ONE_MEGABYTE, 'vgroup')

        created_lv, = lvsystem.get_logical_volumes_with_name('volume')

        self.assertEquals(100, created_lv.size_mb)

        self.assertTrue(created_lv.active)
        self.assertTrue(created_lv.zeroed)

    @with_lvm_subsystem
    def test_create_volume_is_zeroed(self, lvsystem):
        lvsystem.add_volume_group('vgroup')

        lvutil.create('volume', 100 * ONE_MEGABYTE, 'vgroup')

        created_lv, = lvsystem.get_logical_volumes_with_name('volume')

        self.assertEquals(100, created_lv.size_mb)

        self.assertTrue(created_lv.zeroed)

    @with_lvm_subsystem
    def test_create_creates_logical_volume_with_tags(self, lvsystem):
        lvsystem.add_volume_group('vgroup')

        lvutil.create('volume', ONE_MEGABYTE, 'vgroup', tag='hello')

        created_lv, = lvsystem.get_logical_volumes_with_name('volume')
        self.assertEquals('hello', created_lv.tag)

    @with_lvm_subsystem
    def test_create_creates_logical_volume_inactive(self, lvsystem):
        lvsystem.add_volume_group('vgroup')

        lvutil.create('volume', ONE_MEGABYTE, 'vgroup', activate=False)

        created_lv, = lvsystem.get_logical_volumes_with_name('volume')
        self.assertFalse(created_lv.active)

    @with_lvm_subsystem
    def test_create_inactive_volume_is_not_zeroed(self, lvsystem):
        lvsystem.add_volume_group('vgroup')

        lvutil.create('volume', ONE_MEGABYTE, 'vgroup', activate=False)

        created_lv, = lvsystem.get_logical_volumes_with_name('volume')
        self.assertFalse(created_lv.zeroed)

    @mock.patch('util.pread2')
    def test_create_percentage_has_precedence_over_size(self, mock_pread2):
        lvutil.create('volume', ONE_MEGABYTE, 'vgroup',
                      size_in_percentage="10%F")

        mock_pread2.assert_called_once_with(
            [lvutil.CMD_LVCREATE] + "-n volume -l 10%F vgroup".split())


class TestRemove(unittest.TestCase):
    @with_lvm_subsystem
    def test_remove_removes_volume(self, lvsystem):
        lvsystem.add_volume_group('vgroup')
        lvsystem.get_volume_group('vgroup').add_volume('volume', 100)

        lvutil.remove('vgroup/volume')

        self.assertEquals([], lvsystem.get_logical_volumes_with_name('volume'))

    @mock.patch('lvutil._lvmBugCleanup')
    @mock.patch('util.pread2')
    def test_remove_additional_config_param(self, mock_pread2, _bugCleanup):
        lvutil.remove('vgroup/volume', config_param="blah")
        mock_pread2.assert_called_once_with(
            [lvutil.CMD_LVREMOVE]
            + "-f vgroup/volume --config devices{blah}".split()
        )
