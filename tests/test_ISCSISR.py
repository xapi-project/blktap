import unittest
import ISCSI_base
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


class NonLoadingISCSISR(ISCSI_base.BaseISCSISR):
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


class NonInitingISCSISR(ISCSI_base.BaseISCSISR):
    def __init__(self, extra_dconf=None):
        self.mpath = "false"
        self.dconf = {
            'target': 'target',
            'localIQN': 'localIQN',
            'targetIQN': 'targetIQN'
        }

        self.dconf.update(extra_dconf or {})


class NonInitingMultiLUNISCSISR(ISCSI_base.BaseISCSISR):
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

    @mock.patch('ISCSI_base.iscsilib.discovery')
    @mock.patch('ISCSI_base.iscsilib.ensure_daemon_running_ok')
    @mock.patch('ISCSI_base.util._testHost')
    @mock.patch('ISCSI_base.util._convertDNS')
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

    @mock.patch('ISCSI_base.os.path.exists')
    @mock.patch('ISCSI_base.iscsilib.get_node_records')
    def test_initPaths_actual_path_is_active(
            self,
            mock_get_node_records,
            mock_exists):
        mock_get_node_records.return_value = self.node_records
        mock_exists.return_value = True

        iscsi_sr = NonInitingMultiLUNISCSISR(self.node1, self.node2)

        iscsi_sr._initPaths()

        self.assertActiveNodeEquals(self.node1, iscsi_sr)

    @mock.patch('ISCSI_base.os.path.exists')
    @mock.patch('ISCSI_base.iscsilib.get_node_records')
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


class TestISCSISR(TestBase):

    @mock.patch('ISCSI_base.util._convertDNS')
    def test_load_assert_utf_8_chap_credencials(
            self,
            mock__convertDNS):

        """ Asserts that CHAP credentials are always encoded in UTF-8.

            Xapi passes CHAP credentials to ISCSISR as strings of type 'str',
            if they strictly contain ASCII characters, or as strings of type
            'unicode' if they contain at least one non-ASCII character.
        """

        s1 =  'ascii'
        s2 = u'\u03bc\u03b9x\u03b5d'  # == 'mixed' in Greek and Latin chars
        s3 = u'\u03c4\u03bf\u03c0\u03b9\u03ba\u03cc'  # == 'local' in Greek
        s4 = u'\u6c5fw\u6708\u03c2\xfc\xe4\xd6'  # gibberish var char len str

        # These are the sizes of the 4 strings
        # in bytes when encoded in UTF-8
        s1_size = 5
        s2_size = 8
        s3_size = 12
        s4_size = 15

        iscsi_sr = NonInitingISCSISR({
                       'chapuser': s1,
                       'chappassword': s2,
                       'incoming_chapuser': s3,
                       'incoming_chappassword': s4
                   })

        iscsi_sr.load(None)

        self.assertEqual(iscsi_sr.chapuser, s1.encode('utf-8'))
        self.assertEqual(iscsi_sr.chappassword, s2.encode('utf-8'))
        self.assertEqual(iscsi_sr.incoming_chapuser, s3.encode('utf-8'))
        self.assertEqual(iscsi_sr.incoming_chappassword, s4.encode('utf-8'))
        self.assertEqual(len(iscsi_sr.chapuser), s1_size)
        self.assertEqual(len(iscsi_sr.chappassword), s2_size)
        self.assertEqual(len(iscsi_sr.incoming_chapuser), s3_size)
        self.assertEqual(len(iscsi_sr.incoming_chappassword), s4_size)
