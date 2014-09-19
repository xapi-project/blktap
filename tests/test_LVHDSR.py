import unittest
import mock
import LVHDSR
import journaler
import lvhdutil

class SMlog(object):
    def __call__(self, *args):
        print args[0]

class FakeLVHDSR(LVHDSR.LVHDSR):

    def __init__self(self, srcmd, uuid):
        super(FakeLVHDSR, self).__init__(srcmd, uuid)

class TestLVHDSR(unittest.TestCase):

    def create_LVHDSR(self):
        srcmd = mock.Mock()
        srcmd.dconf = {'device': '/dev/bar'}
        srcmd.params = {'command': 'foo', 'session_ref': 'some session ref'}
        return FakeLVHDSR(srcmd, "some SR UUID")

    @mock.patch('lvhdutil.getVDIInfo')
    def test_loadvids(self, mock_getVDIInfo):
        vdi_uuid = 'some VDI UUID'
        mock_getVDIInfo.return_value = {vdi_uuid: lvhdutil.VDIInfo(vdi_uuid)}
        sr = self.create_LVHDSR()
        sr._loadvdis()

    @mock.patch('XenAPI.xapi_local')
    @mock.patch('journaler.Journaler.remove')
    @mock.patch('lvhdutil.lvRefreshOnAllSlaves')
    @mock.patch('util.zeroOut')
    @mock.patch('lvmcache.LVMCache')
    @mock.patch('util.SMlog', new_callable=SMlog)
    @mock.patch('lvhdutil.deflate')
    @mock.patch('lvhdutil.getVDIInfo')
    @mock.patch('journaler.Journaler.getAll')
    def test_undoAllInflateJournals(self, mock_getAll, mock_getVDIInfo,
            mock_lvhdutil_deflate, mock_smlog, mock_lvmcache,
            mock_util_zeroOut, mock_lvhdutil_lvRefreshOnAllSlaves,
            mock_journal_remove, mock_xapi):
        """Test that cleaning up a journal on a local LVHD SR doesn't result in LV refresh calls in other hosts."""

        vdi_uuid = 'some VDI UUID'

        mock_getAll.return_value = {vdi_uuid: '0'}
        mock_getVDIInfo.return_value = {vdi_uuid: lvhdutil.VDIInfo(vdi_uuid)}

        sr = self.create_LVHDSR()

        sr._undoAllInflateJournals()
        self.assertEquals(0, mock_lvhdutil_lvRefreshOnAllSlaves.call_count,
                "cleaning up journals on a local SR shouldn't result in any "
                "update on other hosts")
