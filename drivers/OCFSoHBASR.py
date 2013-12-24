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
# OCFSoHBASR: OCFS over Hardware HBA LUN driver, e.g. Fibre Channel or hardware
# based iSCSI
#

import SR, VDI, OCFSSR, SRCommand, devscan, HBASR
import util
import os, sys, re
import xs_errors
import xmlrpclib
import mpath_cli

CAPABILITIES = ["SR_PROBE", "SR_UPDATE", "SR_METADATA", "VDI_CREATE",
                "VDI_DELETE", "VDI_ATTACH", "VDI_DETACH",
                "VDI_GENERATE_CONFIG", "VDI_CLONE", "VDI_SNAPSHOT",
                "VDI_RESIZE", "ATOMIC_PAUSE", "VDI_UPDATE"]

CONFIGURATION = [ [ 'SCSIid', 'The scsi_id of the destination LUN' ] ]

DRIVER_INFO = {
    'name': 'OCFS over FC',
    'description': 'SR plugin which represents disks as Files on an HBA LUN, e.g. hardware-based iSCSI or FC support',
    'vendor': 'Citrix Systems Inc',
    'copyright': '(C) Citrix Systems Inc',
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
        if self.mpath == "true":
            self.mpathmodule.refresh(self.SCSIid,0)
        self._pathrefresh(OCFSoHBASR)
        try:
            super(OCFSoHBASR, self).create(sr_uuid, size)
        finally:
            if self.mpath == "true":
                self.mpathmodule.reset(self.SCSIid, True) # explicit unmap
                util.remove_mpathcount_field(self.session, self.host_ref, \
                                             self.sr_ref, self.SCSIid)

    def attach(self, sr_uuid):
        self.hbasr.attach(sr_uuid)
        if self.mpath == "true":
            self.mpathmodule.refresh(self.SCSIid,0)
            # set the device mapper's I/O scheduler
            path = '/dev/disk/by-scsid/%s' % self.dconf['SCSIid']
            for file in os.listdir(path):
                self.block_setscheduler('%s/%s' % (path,file))

        self._pathrefresh(OCFSoHBASR)
        if not os.path.exists(self.dconf['device']):
            # Force a rescan on the bus
            self.hbasr._init_hbadict()
            # Must re-initialise the multipath node
            if self.mpath == "true":
                self.mpathmodule.refresh(self.SCSIid,0)

        super(OCFSoHBASR, self).attach(sr_uuid)
        self._setMultipathableFlag(SCSIid=self.SCSIid)

    def scan(self, sr_uuid):
        # During a reboot, scan is called ahead of attach, which causes the MGT
        # to point of the wrong device instead of dm-x. Running multipathing 
        # will take care of this scenario.
        if self.mpath == "true":
            if not os.path.exists(self.dconf['device']):
                util.SMlog("%s path does not exists" % self.dconf['device'])
                self.mpathmodule.refresh(self.SCSIid,0)
                self._pathrefresh(OCFSoHBASR)
                self._setMultipathableFlag(SCSIid=self.SCSIid)
        super(OCFSoHBASR, self).scan(sr_uuid)

    def probe(self):
        if self.mpath == "true" and self.dconf.has_key('SCSIid'):
            # When multipathing is enabled, since we don't refcount the 
            # multipath maps, we should not attempt to do the iscsi.attach/
            # detach when the map is already present, as this will remove it 
            # (which may well be in use).
            maps = []
            try:
                maps = mpath_cli.list_maps()
            except:
                pass

            if self.dconf['SCSIid'] in maps:
                raise xs_errors.XenError('SRInUse')
            self.mpathmodule.refresh(self.SCSIid,0)
        try:
            self._pathrefresh(OCFSoHBASR)
            result = super(OCFSoHBASR, self).probe()
            if self.mpath == "true":
                self.mpathmodule.reset(self.SCSIid,True)
            return result
        except:
           if self.mpath == "true":
                self.mpathmodule.reset(self.SCSIid,True)
           raise

    def detach(self, sr_uuid):
        super(OCFSoHBASR, self).detach(sr_uuid)
        self.mpathmodule.reset(self.SCSIid,True,True) # explicit_unmap
        try:
            pbdref = util.find_my_pbd(self.session, self.host_ref, self.sr_ref)
        except:
            pass
        for key in ["mpath-" + self.SCSIid, "multipathed", "MPPEnabled"]:
            try:
                self.session.xenapi.PBD.remove_from_other_config(pbdref, key)
            except:
                pass

    def delete(self, sr_uuid):
        try:
            super(OCFSoHBASR, self).delete(sr_uuid)
        finally:
            if self.mpath == "true":
                self.mpathmodule.reset(self.SCSIid, True) # explicit unmap

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
        util.SMlog("OCFSoHBAVDI.attach_from_config")
        self.sr.hbasr.attach(sr_uuid)
        if self.sr.mpath == "true":
            self.sr.mpathmodule.refresh(self.sr.SCSIid,0)
        try:
            return self.attach(sr_uuid, vdi_uuid)
        except:
            util.logException("OCFSoHBAVDI.attach_from_config")
            raise xs_errors.XenError('SRUnavailable', \
                        opterr='Unable to attach the heartbeat disk')

def match_scsidev(s):
    regex = re.compile("^/dev/disk/by-id|^/dev/mapper")
    return regex.search(s, 0)
    
if __name__ == '__main__':
    SRCommand.run(OCFSoHBASR, DRIVER_INFO)
else:
    SR.registerSR(OCFSoHBASR)
