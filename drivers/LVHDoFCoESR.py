#!/usr/bin/python
#
# Copyright (C) Citrix Systems Inc.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published
# by the Free Software Foundation; version 2.1 only.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
#
# LVHDoFCoESR: LVHD over Fibre Channel over Ethernet driver
#

import SR
import LVHDoHBASR
import LVHDSR
import SRCommand
import sys
import xs_errors
import util

CAPABILITIES = ["SR_PROBE", "SR_UPDATE", "SR_METADATA", "SR_TRIM",
                "VDI_CREATE", "VDI_DELETE", "VDI_ATTACH", "VDI_DETACH",
                "VDI_GENERATE_CONFIG", "VDI_SNAPSHOT", "VDI_CLONE",
                "VDI_RESIZE", "ATOMIC_PAUSE", "VDI_RESET_ON_BOOT/2",
                "VDI_UPDATE"]

CONFIGURATION = [['SCSIid', 'The scsi_id of the destination LUN'],
                ['allocation', 'Valid values are thick or thin(optional,\
                 defaults to thick)']]

DRIVER_INFO = {
    'name': 'LVHD over FCoE',
    'description': 'SR plugin which represents disks as VHDs on Logical \
    Volumes within a Volume Group created on a FCoE LUN',
    'vendor': 'Citrix Systems Inc',
    'copyright': '(C) 2015 Citrix Systems Inc',
    'driver_version': '1.0',
    'required_api_version': '1.0',
    'capabilities': CAPABILITIES,
    'configuration': CONFIGURATION
}


class LVHDoFCoESR(LVHDoHBASR.LVHDoHBASR):

    """LVHD over FCoE storage repository"""
    def handles(type):
        if __name__ == '__main__':
            name = sys.argv[0]
        else:
            name = __name__
        if name.endswith("LVMoFCoESR"):
            return type == "lvmofcoe"  # for the initial switch from LVM
        if type == "lvhdofcoe":
            return True
        return False
    handles = staticmethod(handles)

    def load(self, sr_uuid):
        driver = SR.driver('hba')
        if 'type' not in self.original_srcmd.params['device_config'] or \
                'type' in self.original_srcmd.params['device_config'] and \
                self.original_srcmd.dconf['type'] == "any":
            self.original_srcmd.dconf['type'] = "fcoe"
        self.hbasr = driver(self.original_srcmd, sr_uuid)
        pbd = None
        try:
            pbd = util.find_my_pbd(self.session, self.host_ref, self.sr_ref)
        except:
            pass

        if not self.dconf.has_key('SCSIid') or not self.dconf['SCSIid']:
            print >>sys.stderr, self.hbasr.print_devs()
            raise xs_errors.XenError('ConfigSCSIid')

        self.SCSIid = self.dconf['SCSIid']
        self._pathrefresh(LVHDoFCoESR)
        LVHDSR.LVHDSR.load(self, sr_uuid)

    def vdi(self, uuid):
        return LVHDoFCoEVDI(self, uuid)


class LVHDoFCoEVDI(LVHDoHBASR.LVHDoHBAVDI):
    pass

if __name__ == '__main__':
    SRCommand.run(LVHDoFCoESR, DRIVER_INFO)
else:
    SR.registerSR(LVHDoFCoESR)
