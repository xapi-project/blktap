import mock
import nfs
import ISOSR
import unittest


class FakeISOSR(ISOSR.ISOSR):
    uuid = None
    sr_ref = None
    session = None
    srcmd = None

    def __init__(self, srcmd, none):
        self.dconf = srcmd.dconf
        self.srcmd = srcmd


class TestISOSR(unittest.TestCase):

    def create_isosr(self, location='aServer:/aLocation', atype=None,
                     sr_uuid='asr_uuid', nfsversion=None):
        srcmd = mock.Mock()
        srcmd.dconf = {
            'location': location
        }
        if atype:
            srcmd.dconf.update({'type': atype})
        if nfsversion:
            srcmd.dconf.update({'nfsversion': nfsversion})
        srcmd.params = {
            'command': 'some_command'
        }
        isosr = FakeISOSR(srcmd, None)
        isosr.load(sr_uuid)
        return isosr

    def test_load(self):
        self.create_isosr()

    @mock.patch('nfs.validate_nfsversion')
    def test_load_validate_nfsversion_called(self, validate_nfsversion):
        isosr = self.create_isosr(nfsversion='aNfsversion')

        validate_nfsversion.assert_called_once_with('aNfsversion')

    @mock.patch('NFSSR.Lock')
    @mock.patch('nfs.validate_nfsversion')
    def test_load_validate_nfsversion_returnused(self, validate_nfsversion,
                                                 Lock):
        validate_nfsversion.return_value = 'aNfsversion'

        self.assertEquals(self.create_isosr().nfsversion, 'aNfsversion')

    @mock.patch('NFSSR.Lock')
    @mock.patch('nfs.validate_nfsversion')
    def test_load_validate_nfsversion_exceptionraised(self,
                                                      validate_nfsversion,
                                                      Lock):
        validate_nfsversion.side_effect = nfs.NfsException('aNfsException')

        self.assertRaises(nfs.NfsException, self.create_isosr)

    @mock.patch('util.gen_uuid')
    @mock.patch('nfs.soft_mount')
    @mock.patch('util._convertDNS')
    @mock.patch('nfs.validate_nfsversion')
    @mock.patch('util.makedirs')
    @mock.patch('ISOSR.ISOSR._checkmount')
    def test_attach_nfs(self, _checkmount, makedirs, validate_nfsversion,
                        convertDNS, soft_mount, gen_uuid):
        validate_nfsversion.return_value = 'aNfsversionChanged'
        isosr = self.create_isosr(location='aServer:/aLocation', atype='nfs',
                                  sr_uuid='asr_uuid')
        _checkmount.side_effect = [False, True]
        gen_uuid.return_value = 'aUuid'

        isosr.attach(None)

        soft_mount.assert_called_once_with('/var/run/sr-mount/asr_uuid',
                                           'aServer',
                                           '/aLocation',
                                           'tcp',
                                           nfsversion='aNfsversionChanged')
