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

import SR, VDI, SRCommand, util
import os, re
import xs_errors
import xmlrpclib
import string

CAPABILITIES = ["VDI_ATTACH", "VDI_DETACH", "VDI_CLONE", "VDI_SNAPSHOT",
                "SR_SCAN", "SR_ATTACH", "SR_DETACH"]
CONFIGURATION = ['location', '/dev/shm subdirectory']
DRIVER_INFO = {
    'name': 'SHM',
    'description': 'Handles shared memory virtual disks',
    'vendor': 'Citrix Systems Inc.',
    'copyright': '(c) 2009 Citrix Systems, Inc.',
    'driver_version': '1.0',
    'required_api_version': '1.0',
    'capabilities': CAPABILITIES,
    'configuration': CONFIGURATION
    }

TYPE = "shm"

class SHMSR(SR.SR):
    """Shared memory storage repository"""

    def _loadvdis(self):
        """Scan the location directory."""
        if self.vdis:
            return

        try:
            for name in util.listdir(self.dconf['location']):
                if name != "":
                    self.vdis[name] = SHMVDI(self, util.gen_uuid(), name)
        except:
            pass
        
    def handles(type):
        """Do we handle this type?"""
        if type == TYPE:
            return True
        return False
    handles = staticmethod(handles)

    def content_type(self, sr_uuid):
        """Returns the content_type XML""" 
        return super(SHMSR, self).content_type(sr_uuid)

    def vdi(self, uuid):
        """Create a VDI class"""
	if self.srcmd.params.has_key('vdi_location'):
            return SHMVDI(self, uuid, self.srcmd.params['vdi_location'])
        else:
            return SHMVDI(self, uuid, self.srcmd.params['device_config']['location'])

    def load(self, sr_uuid):
        """Initialises the SR"""
        if not self.dconf.has_key('location'):
            raise xs_errors.XenError('ConfigLocationMissing')

        self.sr_vditype = 'file'
        self.physical_size = 0
        self.physical_utilisation = 0
        self.virtual_allocation = 0

    def attach(self, sr_uuid):
        """Std. attach"""
	self._loadvdis()

    def detach(self, sr_uuid):
        """Std. detach"""
	pass

    def scan(self, sr_uuid):
        """Scan"""
	self._loadvdis()
        return super(SHMSR, self).scan(sr_uuid)

    def create(self, sr_uuid, size):
        self.attach(sr_uuid)
        self.detach(sr_uuid)


class SHMVDI(VDI.VDI):
    def load(self, vdi_uuid):
        try:
            stat = os.stat(self.path)
            self.utilisation = long(stat.st_size)
            self.size = long(stat.st_size)
        except:
            pass

    def __init__(self, mysr, uuid, filename):
        self.uuid = uuid
        self.path = os.path.join(mysr.dconf['location'], filename)
        VDI.VDI.__init__(self, mysr, None)
        self.label = filename
        self.location = filename
        self.vdi_type = 'file'
        self.read_only = True
        self.shareable = True
        self.sm_config = {}

    def detach(self, sr_uuid, vdi_uuid):
	pass

    def clone(self, sr_uuid, vdi_uuid):
        return self.get_params()

    def snapshot(self, sr_uuid, vdi_uuid):
        return self.get_params()

if __name__ == '__main__':
    SRCommand.run(SHMSR, DRIVER_INFO)
else:
    SR.registerSR(SHMSR)
