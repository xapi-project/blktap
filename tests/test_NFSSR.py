import mock
import nfs
import NFSSR
import unittest


class FakeNFSSR(NFSSR.NFSSR):
    uuid = None
    sr_ref = None
    session = None
    srcmd = None

    def __init__(self, srcmd, none):
        self.dconf = srcmd.dconf
        self.srcmd = srcmd


class TestNFSSR(unittest.TestCase):

    def create_nfssr(self, server='aServer', serverpath='/aServerpath',
                     sr_uuid='asr_uuid'):
        srcmd = mock.Mock()
        srcmd.dconf = {
            'server': server,
            'serverpath': serverpath
        }
        srcmd.params = {
            'command': 'some_command'
        }
        nfssr = FakeNFSSR(srcmd, None)
        nfssr.load(sr_uuid)
        return nfssr

    @mock.patch('NFSSR.Lock')
    def test_load(self, Lock):
        self.create_nfssr()

    @mock.patch('util.makedirs')
    @mock.patch('NFSSR.Lock')
    @mock.patch('nfs.soft_mount')
    @mock.patch('util._testHost')
    @mock.patch('nfs.check_server_tcp')
    def test_attach(self, check_server_tcp, _testhost, soft_mount, Lock,
                    makedirs):
        nfssr = self.create_nfssr(server='aServer', serverpath='/aServerpath',
                                  sr_uuid='UUID')

        nfssr.attach(None)

        check_server_tcp.assert_called_once_with('aServer')
        soft_mount.assert_called_once_with('/var/run/sr-mount/UUID',
                                           'aServer',
                                           '/aServerpath/UUID',
                                           'tcp',
                                           timeout=0)
