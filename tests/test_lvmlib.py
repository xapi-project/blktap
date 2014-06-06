import unittest
import mock

import lvmlib


class ExecResultMixIn(object):
    def assertExecutionSucceeded(self, exec_result):
        returncode, stdout, stderr = exec_result

        self.assertEquals(0, returncode)

    def assertExecutionFailed(self, exec_result):
        returncode, stdout, stderr = exec_result

        self.assertEquals(1, returncode)


class TestLVSubSystem(unittest.TestCase, ExecResultMixIn):
    def test_lvcreate_is_mocked(self):
        executable_injector = mock.Mock()

        lvsubsystem = lvmlib.LVSubsystem(None, executable_injector)

        self.assertTrue(
            mock.call('/usr/sbin/lvcreate', lvsubsystem.fake_lvcreate)
            in executable_injector.mock_calls
        )

    def test_lvremove_is_mocked(self):
        executable_injector = mock.Mock()

        lvsubsystem = lvmlib.LVSubsystem(None, executable_injector)

        self.assertTrue(
            mock.call('/usr/sbin/lvremove', lvsubsystem.fake_lvremove)
            in executable_injector.mock_calls
        )

    def test_dmsetup_is_mocked(self):
        executable_injector = mock.Mock()

        lvsubsystem = lvmlib.LVSubsystem(None, executable_injector)

        self.assertTrue(
            mock.call('/sbin/dmsetup', lvsubsystem.fake_dmsetup)
            in executable_injector.mock_calls
        )

    def test_add_volume_group(self):
        lvsubsystem = lvmlib.LVSubsystem(None, mock.Mock())

        lvsubsystem.add_volume_group('vg')
        vg = lvsubsystem.get_volume_group('vg')

        self.assertEquals('vg', vg.name)

    def test_fake_lvcreate_creates_volume(self):
        lvsubsystem = lvmlib.LVSubsystem(mock.Mock(), mock.Mock())
        vg = lvsubsystem.add_volume_group('vg')

        exec_result = lvsubsystem.fake_lvcreate(
            "someprog -n name -L 100 vg".split(), '')

        lv, = lvsubsystem.get_logical_volumes_with_name('name')

        self.assertEquals('name', lv.name)
        self.assertEquals(lvsubsystem.get_volume_group('vg'), lv.volume_group)
        self.assertTrue(lv.active)
        self.assertTrue(lv.zeroed)
        self.assertEquals(None, lv.tag)
        self.assertEquals(100, lv.size_mb)

    def test_fake_lvcreate_with_tags(self):
        lvsubsystem = lvmlib.LVSubsystem(mock.Mock(), mock.Mock())
        lvsubsystem.add_volume_group('vg')

        exec_result = lvsubsystem.fake_lvcreate(
            "someprog -n name --addtag tagg -L 100 vg".split(), '')

        lv, = lvsubsystem.get_logical_volumes_with_name('name')
        self.assertEquals('tagg', lv.tag)

    def test_fake_lvcreate_inactive(self):
        lvsubsystem = lvmlib.LVSubsystem(mock.Mock(), mock.Mock())
        lvsubsystem.add_volume_group('vg')

        exec_result = lvsubsystem.fake_lvcreate(
            "someprog -n name --inactive -L 100 vg".split(), '')

        lv, = lvsubsystem.get_logical_volumes_with_name('name')
        self.assertFalse(lv.active)

    def test_fake_lvcreate_non_zeroed(self):
        lvsubsystem = lvmlib.LVSubsystem(mock.Mock(), mock.Mock())
        lvsubsystem.add_volume_group('vg')

        exec_result = lvsubsystem.fake_lvcreate(
            "someprog -n name --zero n -L 100 vg".split(), '')

        lv, = lvsubsystem.get_logical_volumes_with_name('name')

        self.assertFalse(lv.zeroed)
        self.assertExecutionSucceeded(exec_result)

    def test_fake_lvcreate_called_with_wrong_params(self):
        lvsubsystem = lvmlib.LVSubsystem(mock.Mock(), mock.Mock())
        lvsubsystem.add_volume_group('vg')

        exec_result = lvsubsystem.fake_lvcreate(
            "someprog --something-stupid -n name n -L 100 vg".split(), '')

        self.assertExecutionFailed(exec_result)

    def test_fake_lvcreate_fails_if_no_volume_group_found(self):
        lvsubsystem = lvmlib.LVSubsystem(mock.Mock(), mock.Mock())

        exec_result = lvsubsystem.fake_lvcreate(
            "someprog -n name -L 100 nonexisting".split(), '')

        self.assertExecutionFailed(exec_result)

    def test_fake_lvremove(self):
        lvsubsystem = lvmlib.LVSubsystem(mock.Mock(), mock.Mock())
        lvsubsystem.add_volume_group('vg')
        lvsubsystem.get_volume_group('vg').add_volume('lv', 100)

        exec_result = lvsubsystem.fake_lvremove(
            "someprog vg/lv".split(), '')

        self.assertExecutionSucceeded(exec_result)

    def test_fake_lvremove_with_force(self):
        lvsubsystem = lvmlib.LVSubsystem(mock.Mock(), mock.Mock())
        lvsubsystem.add_volume_group('vg')
        lvsubsystem.get_volume_group('vg').add_volume('lv', 100)

        exec_result = lvsubsystem.fake_lvremove(
            "someprog -f vg/lv".split(), '')

        self.assertExecutionSucceeded(exec_result)

    def test_fake_lvremove_with_bad_params(self):
        lvsubsystem = lvmlib.LVSubsystem(mock.Mock(), mock.Mock())
        lvsubsystem.add_volume_group('vg')
        lvsubsystem.get_volume_group('vg').add_volume('lv', 100)

        exec_result = lvsubsystem.fake_lvremove(
            "someprog -f vg/lv --stupid-parameter".split(), '')

        self.assertExecutionFailed(exec_result)

    def test_fake_dmsetup_status_returns_zero(self):
        lvsubsystem = lvmlib.LVSubsystem(mock.Mock(), mock.Mock())

        exec_result = lvsubsystem.fake_dmsetup(
            "someprog status".split(), '')

        self.assertExecutionSucceeded(exec_result)


