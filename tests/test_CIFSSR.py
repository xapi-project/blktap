import unittest
import mock
import CIFSSR
import xs_errors
import XenAPI
import vhdutil
import util
import errno

class FakeCIFSSR(CIFSSR.CIFSSR):
    uuid = None
    sr_ref = None
    mountpoint = None
    linkpath = None
    path = None
    session = None
    remoteserver = None

    def __init__(self, srcmd, none):
        self.dconf = srcmd.dconf
        self.srcmd = srcmd
        self.uuid = 'auuid'
        self.sr_ref = 'asr_ref'
        self.mountpoint = 'aMountpoint'
        self.linkpath = 'aLinkpath'
        self.path = 'aPath'
        self.remoteserver = 'aRemoteserver'

class Test_CIFSSR(unittest.TestCase):

    def create_cifssr(self, sr_uuid='asr_uuid', server='\\aServer', serverpath = '/aServerpath', username = 'aUsername', password = 'aPassword'):
        srcmd = mock.Mock()
        srcmd.dconf = {
            'server': server,
            'serverpath': serverpath,
            'username': username,
            'password': password
        }
        srcmd.params = {
            'command': 'some_command',
            'device_config': {}
        }
        cifssr = FakeCIFSSR(srcmd, None)
        cifssr.load(sr_uuid)
        return cifssr

    #Attach
    @mock.patch('CIFSSR.CIFSSR.checkmount')
    @mock.patch('CIFSSR.CIFSSR.mount')
    def test_attach_cifsexception_raises_xenerror(self, mock_mount, mock_checkmount):
        cifssr = self.create_cifssr()
        mock_mount = mock.Mock(side_effect=CIFSSR.CifsException("mount raised CifsException"))
        mock_checkmount = mock.Mock(return_value=False)
        try:
            cifssr.attach('asr_uuid')
        except Exception, exc:
            self.assertTrue(isinstance(exc,xs_errors.XenError))

    @mock.patch('CIFSSR.CIFSSR.checkmount')
    def test_attach_if_mounted_then_attached(self, mock_checkmount):
        cifssr = self.create_cifssr()
        mock_checkmount = mock.Mock(return_value=True)
        cifssr.attach('asr_uuid')
        self.assertTrue(cifssr.attached)

    #Detach
    @mock.patch('CIFSSR.CIFSSR.unmount')
    def test_detach_cifsexception_raises_xenerror(self,mock_unmount):
        cifssr = self.create_cifssr()
        mock_unmount = mock.Mock(side_effect=CIFSSR.CifsException("unmount raised CifsException"))
        try:
            cifssr.detach('asr_uuid')
        except Exception, exc:
            self.assertTrue(isinstance(exc,xs_errors.XenError))

    @mock.patch('CIFSSR.CIFSSR.checkmount',return_value=False)
    def test_detach_not_detached_if_not_mounted(self, mock_checkmount):
        cifssr = self.create_cifssr()
        cifssr.attached = True
        mock_checkmount = mock.Mock(return_value=False)
        cifssr.detach('asr_uuid')
        self.assertTrue(cifssr.attached)

    #Mount
    @mock.patch('util.isdir')
    def test_mount_mountpoint_isdir(self, mock_isdir):
        mock_isdir = mock.Mock(side_effect=util.CommandException(errno.EIO, "Not a directory"))
        cifssr = self.create_cifssr()
        try:
            cifssr.mount()
        except Exception, exc:
            self.assertTrue(isinstance(exc,CIFSSR.CifsException))

    def test_mount_mountpoint_empty_string(self):
        cifssr = self.create_cifssr()
        self.assertRaises(CIFSSR.CifsException, cifssr.mount, "")
