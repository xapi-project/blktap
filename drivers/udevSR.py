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
# udevSR: represents VDIs which are hotplugged into dom0 via udev e.g.
#         USB CDROM/disk devices

import SR, VDI, SRCommand, util, lvutil
import errno
import os, sys, time, stat
import xml.dom.minidom
import xmlrpclib
import xs_errors
import sysdevice

CAPABILITIES = [ "VDI_INTRODUCE","VDI_ATTACH","VDI_DETACH","VDI_UPDATE", "SR_UPDATE" ]

CONFIGURATION = \
    [ [ 'location', 'path to mount (required) (e.g. server:/path)' ] ]

DRIVER_INFO = {
    'name': 'udev',
    'description': 'SR plugin which represents devices plugged in via udev as VDIs',
    'vendor': 'Citrix Systems Inc',
    'copyright': '(C) 2008 Citrix Systems Inc',
    'driver_version': '1.0',
    'required_api_version': '1.1',
    'capabilities': CAPABILITIES,
    'configuration': CONFIGURATION
    }

TYPE = 'udev'

class udevSR(SR.SR):
    """udev-driven storage repository"""
    def handles(type):
        if type == TYPE:
            return True
        return False
    handles = staticmethod(handles)

    def type(self, sr_uuid):
        return super(udevSR, self).type(sr_uuid)

    def vdi(self, uuid):
        util.SMlog("params = %s" % (self.srcmd.params.keys()))
        return udevVDI(self, self.srcmd.params['vdi_location'])
    
    def load(self, sr_uuid):
        # First of all, check we've got the correct keys in dconf
        if not self.dconf.has_key('location'):        
            raise xs_errors.XenError('ConfigLocationMissing')
        self.sr_vditype = 'phy'
        # Cache the sm_config 
        self.sm_config = self.session.xenapi.SR.get_sm_config(self.sr_ref)

    def update(self, sr_uuid):
        # Return as much information as we have
        sr_root =self.dconf['location']

        if util.pathexists(sr_root):
            for filename in os.listdir(sr_root):
                path = os.path.join(sr_root, filename)
                x = udevVDI(self, path)
                self.vdis[path] = x

        the_sum = 0L
        for vdi in self.vdis.values():
            the_sum = the_sum + vdi.size
            
        self.physical_size = the_sum
        self.physical_utilisation = the_sum
        self.virtual_allocation = the_sum

        self._db_update()
    
    def scan(self, sr_uuid):
        self.update(sr_uuid)

        # base class scan does all the work:
        return super(udevSR, self).scan(sr_uuid)

    def create(self, sr_uuid, size):
        pass

    def delete(self, sr_uuid):
        pass

    def attach(self, sr_uuid):
        pass

    def detach(self, sr_uuid):
        pass

def read_whole_file(filename):
    f = open(filename, 'r')
    try:
        return f.readlines()
    finally:
        f.close()

class udevVDI(VDI.VDI):
    def __init__(self, sr, location):
        self.location = location
        VDI.VDI.__init__(self, sr, None)
        
    def load(self, location):
        self.path = self.location
        self.size = 0
        self.utilisation = 0
        self.label = self.path
        self.sm_config = {}
        try:
            s = os.stat(self.path)
            self.deleted = False

            # Use the CTIME of the symlink to mean "time it was hotplugged"
            iso8601 = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(s[stat.ST_CTIME]))
            self.sm_config['hotplugged_at'] = iso8601

            dev = os.path.basename(self.path)
	    info = sysdevice.stat(dev)
	    if "size" in info.keys():
	        self.size = info["size"]
                self.utilisation = self.size

            self.label = "%s %s" % (info["bus"], info["bus_path"])
            self.description = info["hwinfo"]
            
            # XXX: what other information can we recover?
            if self.sr.sm_config.has_key('type'):
                self.read_only = self.sr.sm_config['type'] == "cd"
                
        except OSError, e:
            self.deleted = True
        
    def introduce(self, sr_uuid, vdi_uuid):
        self.uuid = vdi_uuid
        self.location = self.sr.srcmd.params['vdi_location']
        self._db_introduce()
        # Update the physical_utilisation etc
        self.sr.update(sr_uuid)
        return super(udevVDI, self).get_params()

    def update(self, sr_uuid, vdi_location):
        self.load(vdi_location)
        # _db_update requires self.uuid to be set
        self.uuid = self.sr.srcmd.params['vdi_uuid']
        self._db_update()
        # also reset the name-label and description since we're a bit of
        # a special SR
        # this would lead to an infinite loop as VDI.set_name_label now
        # calls VDI.update 
        # temporarily commenting this to pass quicktest
        #vdi = self.sr.session.xenapi.VDI.get_by_uuid(self.uuid)        
        #self.sr.session.xenapi.VDI.set_name_label(vdi, self.label)
        #self.sr.session.xenapi.VDI.set_name_description(vdi, self.description)        

    def attach(self, sr_uuid, vdi_uuid):
        if self.deleted:
            raise xs_errors.XenError('VDIUnavailable')

        return super(udevVDI, self).attach(sr_uuid, vdi_uuid)

    def detach(self, sr_uuid, vdi_uuid):
        pass

if __name__ == '__main__':
    SRCommand.run(udevSR, DRIVER_INFO)
else:
    SR.registerSR(udevSR)
