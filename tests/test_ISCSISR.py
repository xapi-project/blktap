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


class NonInitingMultiLUNISCSISR(ISCSISR.ISCSISR):
    def __init__(self, node1, node2):
        self.mpath = "false"
        self.dconf = {
            'target': node1['ip'],
            'localIQN': 'localIQN',
            'targetIQN': node1['iqn']
        }

        self.dconf.update({})
        self.target = node1['ip']
        self.port = node1['port']
        self.targetIQN = node1['iqn']
        self.attached = True
        self.multihomed = True
        extra_adapter = "%s:%d" % (node2['ip'], node2['port'])
        self.adapter = {
            extra_adapter: None
        }

    def _synchroniseAddrList(self, *args, **kwargs):
        pass

    def _init_adapters(self):
        pass


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


class TestMultiLUNISCSISR(unittest.TestCase):

    def setUp(self):
        self.node1 = {
            'ip': '127.0.0.1',
            'port': 3260,
            'iqn': 'IQN'
        }
        self.node2 = {
            'ip': '127.0.0.2',
            'port': 8080,
            'iqn': 'IQN',
            'tpgt': 'TPGT'
        }
        self.node_records = [(
            "%s:%d" % (self.node2['ip'], self.node2['port']),
            self.node2['tpgt'],
            self.node2['iqn']
        )]

    def assertActiveNodeEquals(self, node, iscsi_sr):
        node_ip_port = "%s:%d" % (node['ip'], node['port'])
        node_path = '/dev/iscsi/%s/%s' % (node['iqn'], node_ip_port)

        self.assertEquals(node_path, iscsi_sr.path)
        self.assertEquals(node_ip_port, iscsi_sr.tgtidx)
        self.assertEquals(node_ip_port, iscsi_sr.address)

    @mock.patch('ISCSISR.os.path.exists')
    @mock.patch('ISCSISR.iscsilib.get_node_records')
    def test_initPaths_actual_path_is_active(
            self,
            mock_get_node_records,
            mock_exists):
        mock_get_node_records.return_value = self.node_records
        mock_exists.return_value = True

        iscsi_sr = NonInitingMultiLUNISCSISR(self.node1, self.node2)

        iscsi_sr._initPaths()

        self.assertActiveNodeEquals(self.node1, iscsi_sr)

    @mock.patch('ISCSISR.os.path.exists')
    @mock.patch('ISCSISR.iscsilib.get_node_records')
    def test_initPaths_active_path_detection(
            self,
            mock_get_node_records,
            mock_exists):
        mock_get_node_records.return_value = self.node_records

        def fake_exists(path):
            if self.node1['ip'] in path:
                return False
            return True

        mock_exists.side_effect = fake_exists

        iscsi_sr = NonInitingMultiLUNISCSISR(self.node1, self.node2)

        iscsi_sr._initPaths()

        self.assertActiveNodeEquals(self.node2, iscsi_sr)
