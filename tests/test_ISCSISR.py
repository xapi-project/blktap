import unittest
import ISCSISR
import mock
import xs_errors
import os


class TestBase(unittest.TestCase):
    """ Provides errorcodes.xml, so exceptions are sensible """

    def setUp(self):
        self._xmldefs = xs_errors.XML_DEFS
        xs_errors.XML_DEFS = os.path.join(
            os.path.dirname(__file__), 'XE_SR_ERRORCODES.xml')

    def tearDown(self):
        xs_errors.XML_DEFS = self._xmldefs


class NonLoadingISCSISR(ISCSISR.ISCSISR):
    def load(self, sr_uuid):
        pass


class TestForceTapDiskConfig(TestBase):

    def _get_iscsi_sr(self, dconf=None):
        srcmd = mock.Mock()
        srcmd.dconf = dconf or {}
        srcmd.params = {
            'command': 'some_command'
        }

        iscsisr = NonLoadingISCSISR(srcmd, None)
        return iscsisr

    def test_default_value(self):
        iscsi_sr = self._get_iscsi_sr()

        self.assertEquals(False, iscsi_sr.force_tapdisk)

    def test_set_to_true(self):
        iscsi_sr = self._get_iscsi_sr({
            'force_tapdisk': 'true'
        })

        self.assertEquals(True, iscsi_sr.force_tapdisk)


class NonInitingISCSISR(ISCSISR.ISCSISR):
    def __init__(self, extra_dconf=None):
        self.mpath = "false"
        self.dconf = {
            'target': 'target',
            'localIQN': 'localIQN',
            'targetIQN': 'targetIQN'
        }

        self.dconf.update(extra_dconf or {})


class TestVdiTypeSetting(TestBase):

    @mock.patch('ISCSISR.iscsilib.discovery')
    @mock.patch('ISCSISR.iscsilib.ensure_daemon_running_ok')
    @mock.patch('ISCSISR.util._testHost')
    @mock.patch('ISCSISR.util._convertDNS')
    def load_iscsi_sr(self, convertDNS, testHost, ensure_daemon_running_ok,
                      discovery, iscsi_sr):
        iscsi_sr.load(None)

    def test_default_vdi_type(self):
        iscsi_sr = NonInitingISCSISR()

        self.load_iscsi_sr(iscsi_sr=iscsi_sr)

        self.assertEquals('phy', iscsi_sr.sr_vditype)

    def test_vdi_type_modified_by_force_tapdisk(self):
        iscsi_sr = NonInitingISCSISR(extra_dconf=dict(force_tapdisk='true'))

        self.load_iscsi_sr(iscsi_sr=iscsi_sr)

        self.assertEquals('aio', iscsi_sr.sr_vditype)
