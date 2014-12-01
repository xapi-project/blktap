import unittest
import udevSR
import SRCommand
import mock


VDI_LOCATION = '/path/to/vdi'


class TestVdi(unittest.TestCase):

    @mock.patch('udevSR.udevSR.get_vdi_location')
    @mock.patch('udevSR.udevSR.load')
    def test_vdi_succeeds_if_vdi_location_not_in_params_dictionary(
            self,
            mock_load,
            mock_get_vdi_location):
        mock_get_vdi_location.return_value = VDI_LOCATION
        srcmd = SRCommand.SRCommand('driver_info')
        srcmd.params = {'command': 'cmd'}
        sr_uuid = 'sr_uuid'
        udev_sr = udevSR.udevSR(srcmd, sr_uuid)

        self.assertEquals(None, udev_sr.srcmd.params.get('vdi_location'))

        udev_vdi = udev_sr.vdi('vdi_uuid')

        self.assertEquals(VDI_LOCATION, udev_vdi.location)
