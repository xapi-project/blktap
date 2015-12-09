import unittest
import testlib
import lvmlib
import mock

import os
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
        lvsystem.add_volume_group('VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7')

        lvutil.create('volume', 100 * ONE_MEGABYTE, 'VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7')

        created_lv, = lvsystem.get_logical_volumes_with_name('volume')

        self.assertEquals(100, created_lv.size_mb)

    @with_lvm_subsystem
    def test_create_volume_is_in_the_right_volume_group(self, lvsystem):
        lvsystem.add_volume_group('VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7')

        lvutil.create('volume', 100 * ONE_MEGABYTE, 'VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7')

        created_lv, = lvsystem.get_logical_volumes_with_name('volume')

        self.assertEquals(100, created_lv.size_mb)

        self.assertEquals('VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7', created_lv.volume_group.name)
        self.assertTrue(created_lv.active)
        self.assertTrue(created_lv.zeroed)

    @with_lvm_subsystem
    def test_create_volume_is_active(self, lvsystem):
        lvsystem.add_volume_group('VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7')

        lvutil.create('volume', 100 * ONE_MEGABYTE, 'VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7')

        created_lv, = lvsystem.get_logical_volumes_with_name('volume')

        self.assertEquals(100, created_lv.size_mb)

        self.assertTrue(created_lv.active)
        self.assertTrue(created_lv.zeroed)

    @with_lvm_subsystem
    def test_create_volume_is_zeroed(self, lvsystem):
        lvsystem.add_volume_group('VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7')

        lvutil.create('volume', 100 * ONE_MEGABYTE, 'VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7')

        created_lv, = lvsystem.get_logical_volumes_with_name('volume')

        self.assertEquals(100, created_lv.size_mb)

        self.assertTrue(created_lv.zeroed)

    @with_lvm_subsystem
    def test_create_creates_logical_volume_with_tags(self, lvsystem):
        lvsystem.add_volume_group('VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7')

        lvutil.create('volume', ONE_MEGABYTE, 'VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7', tag='hello')

        created_lv, = lvsystem.get_logical_volumes_with_name('volume')
        self.assertEquals('hello', created_lv.tag)

    @mock.patch('util.pread')
    def test_create_percentage_has_precedence_over_size(self, mock_pread):
        lvutil.create('volume', ONE_MEGABYTE, 'VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7',
                      size_in_percentage="10%F")

        mock_pread.assert_called_once_with(
            [os.path.join(lvutil.LVM_BIN,lvutil.CMD_LVCREATE)] +
            "-n volume -l 10%F VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7".split(),
            quiet=False)

class TestRemove(unittest.TestCase):
    @with_lvm_subsystem
    def test_remove_removes_volume(self, lvsystem):
        lvsystem.add_volume_group('VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7')
        lvsystem.get_volume_group('VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7').add_volume('volume', 100)

        lvutil.remove('VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7/volume')

        self.assertEquals([], lvsystem.get_logical_volumes_with_name('volume'))

    @mock.patch('lvutil._lvmBugCleanup')
    @mock.patch('util.pread')
    def test_remove_additional_config_param(self, mock_pread, _bugCleanup):
        lvutil.remove('VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7/volume', config_param="blah")
        mock_pread.assert_called_once_with(
            [os.path.join(lvutil.LVM_BIN, lvutil.CMD_LVREMOVE)]
            + "-f VG_XenStorage-b3b18d06-b2ba-5b67-f098-3cdd5087a2a7/volume --config devices{blah}".split(),
           quiet= False)
