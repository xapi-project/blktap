#!/usr/bin/env python
# Copyright (C) 2008-2013 Citrix Ltd.
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
#
# OCFSoHBASR: OCFS over Hardware HBA LUN driver, e.g. Fibre Channel or hardware
# based iSCSI
#

import SR, VDI, OCFSSR, SRCommand, devscan, HBASR
import util, scsiutil
import os, sys, re
import xs_errors
import xmlrpclib

CAPABILITIES = ["SR_PROBE","VDI_CREATE","VDI_DELETE","VDI_ATTACH",
                "VDI_DETACH","VDI_RESIZE","VDI_GENERATE_CONFIG"]

CONFIGURATION = [ [ 'SCSIid', 'The scsi_id of the destination LUN' ] ]

DRIVER_INFO = {
    'name': 'OCFS over FC',
    'description': 'SR plugin which represents disks as Logical Volumes within a Volume Group created on an HBA LUN, e.g. hardware-based iSCSI or FC support',
    'vendor': 'Citrix Systems Inc',
    'copyright': '(C) 2008-2013 Citrix Systems Inc',
    'driver_version': '1.0',
    'required_api_version': '1.0',
    'capabilities': CAPABILITIES,
    'configuration': CONFIGURATION
    }

class OCFSoHBASR(OCFSSR.OCFSSR):
    """OCFS over HBA storage repository"""
    def handles(type):
        if type == "ocfsohba":
            return True
        return False
    handles = staticmethod(handles)

    def load(self, sr_uuid):
        driver = SR.driver('hba')
        self.hbasr = driver(self.original_srcmd, sr_uuid)

        pbd = None
        try:
            pbd = util.find_my_pbd(self.session, self.host_ref, self.sr_ref)
        except:
            pass

        if not self.dconf.has_key('SCSIid') or not self.dconf['SCSIid']:
            print >>sys.stderr,self.hbasr.print_devs()
            raise xs_errors.XenError('ConfigSCSIid')

        self.SCSIid = self.dconf['SCSIid']
        self._pathrefresh(OCFSoHBASR)
        super(OCFSoHBASR, self).load(sr_uuid)

    def create(self, sr_uuid, size):
        self.hbasr.attach(sr_uuid)
        self._pathrefresh(OCFSoHBASR)
        super(OCFSoHBASR, self).create(sr_uuid, size)

    def attach(self, sr_uuid):
        self.hbasr.attach(sr_uuid)
        self._pathrefresh(OCFSoHBASR)
        super(OCFSoHBASR, self).attach(sr_uuid)
        self._setMultipathableFlag(SCSIid=self.SCSIid)

    def vdi(self, uuid, loadLocked=False):
        return OCFSoHBAVDI(self, uuid)
    
class OCFSoHBAVDI(OCFSSR.OCFSFileVDI):
    def generate_config(self, sr_uuid, vdi_uuid):
        dict = {}
        self.sr.dconf['multipathing'] = self.sr.mpath
        self.sr.dconf['multipathhandle'] = self.sr.mpathhandle
        dict['device_config'] = self.sr.dconf
        dict['sr_uuid'] = sr_uuid
        dict['vdi_uuid'] = vdi_uuid
        dict['command'] = 'vdi_attach_from_config'
	# Return the 'config' encoded within a normal XMLRPC response so that
	# we can use the regular response/error parsing code.
	config = xmlrpclib.dumps(tuple([dict]), "vdi_attach_from_config")
        return xmlrpclib.dumps((config,), "", True)

    def attach_from_config(self, sr_uuid, vdi_uuid):
        return super(OCFSoHBAVDI, self).attach(sr_uuid, vdi_uuid)

def match_scsidev(s):
    regex = re.compile("^/dev/disk/by-id|^/dev/mapper")
    return regex.search(s, 0)
    
if __name__ == '__main__':
    SRCommand.run(OCFSoHBASR, DRIVER_INFO)
else:
    SR.registerSR(OCFSoHBASR)
