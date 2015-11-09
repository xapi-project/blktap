import unittest
import mock
import SMBSR
import xs_errors
import XenAPI
import vhdutil
import util
import errno

class FakeSMBSR(SMBSR.SMBSR):
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

class Test_SMBSR(unittest.TestCase):

    def create_smbsr(self, sr_uuid='asr_uuid', server='\\aServer', serverpath = '/aServerpath', username = 'aUsername', password = 'aPassword'):
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
        smbsr = FakeSMBSR(srcmd, None)
        smbsr.load(sr_uuid)
        return smbsr

    #Attach
    @mock.patch('SMBSR.SMBSR.checkmount')
    @mock.patch('SMBSR.SMBSR.mount')
    def test_attach_smbexception_raises_xenerror(self, mock_mount, mock_checkmount):
        smbsr = self.create_smbsr()
        mock_mount = mock.Mock(side_effect=SMBSR.SMBException("mount raised SMBException"))
        mock_checkmount = mock.Mock(return_value=False)
        try:
            smbsr.attach('asr_uuid')
        except Exception, exc:
            self.assertTrue(isinstance(exc,xs_errors.XenError))

    @mock.patch('SMBSR.SMBSR.checkmount')
    def test_attach_if_mounted_then_attached(self, mock_checkmount):
        smbsr = self.create_smbsr()
        mock_checkmount = mock.Mock(return_value=True)
        smbsr.attach('asr_uuid')
        self.assertTrue(smbsr.attached)

    #Detach
    @mock.patch('SMBSR.SMBSR.unmount')
    def test_detach_smbexception_raises_xenerror(self,mock_unmount):
        smbsr = self.create_smbsr()
        mock_unmount = mock.Mock(side_effect=SMBSR.SMBException("unmount raised SMBException"))
        try:
            smbsr.detach('asr_uuid')
        except Exception, exc:
            self.assertTrue(isinstance(exc,xs_errors.XenError))

    @mock.patch('SMBSR.SMBSR.checkmount',return_value=False)
    def test_detach_not_detached_if_not_mounted(self, mock_checkmount):
        smbsr = self.create_smbsr()
        smbsr.attached = True
        mock_checkmount = mock.Mock(return_value=False)
        smbsr.detach('asr_uuid')
        self.assertTrue(smbsr.attached)

    #Mount
    @mock.patch('util.isdir')
    def test_mount_mountpoint_isdir(self, mock_isdir):
        mock_isdir = mock.Mock(side_effect=util.CommandException(errno.EIO, "Not a directory"))
        smbsr = self.create_smbsr()
        try:
            smbsr.mount()
        except Exception, exc:
            self.assertTrue(isinstance(exc,SMBSR.SMBException))

    def test_mount_mountpoint_empty_string(self):
        smbsr = self.create_smbsr()
        self.assertRaises(SMBSR.SMBException, smbsr.mount, "")
