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
# FIXME
# RawHBASR: Hardware HBA LUN driver, e.g. Fibre Channel or SAS or
# hardware based iSCSI
# FIXME

import B_util

import SR, VDI, SRCommand, HBASR, LUNperVDI
import util, scsiutil, devscan
import xs_errors
import os

CAPABILITIES = ["SR_PROBE", "VDI_ATTACH", "VDI_DETACH", "VDI_DELETE"]

CONFIGURATION = [ [ 'type', 'FIXME (optional)' ] ]

DRIVER_INFO = {
    'name': 'RawHBA LUN-per-VDI driver',
    'description': 'SR plugin which represents LUNs as VDIs sourced by hardware HBA adapters, FC support',
    'vendor': 'Citrix Systems Inc',
    'copyright': '(C) 2012 Citrix Systems Inc',
    'driver_version': '1.0',
    'required_api_version': '1.0',
    'capabilities': CAPABILITIES,
    'configuration': CONFIGURATION
    }

NEEDS_LOADVDIS = ["sr_scan"]

TYPE = 'rawhba'

class RawHBASR(HBASR.HBASR):
    """ Raw LUN-per-VDI HBA storage repository"""

    def handles(type):
        return type == TYPE
    handles = staticmethod(handles)

    def load(self, sr_uuid):
        """Extend super class load by initializing the hba dict.
        FIXME: verify this is needed
        """
        super(RawHBASR, self).load(sr_uuid)
        # There are some calls (e.g. probe) where it is not needed at all
        #self._init_hbadict()

        self._get_stats()

        # For this SR the internal self.attached flag hs no meaning
        # at all. Xapi knows about its status and if it is
        # not attached all the commands relying on that are not
        # executed. This variable is used in various places inside the
        # base class so we cannot ignore it. Setting it always to true
        # will prevent sr.create and sr.scan to throw exceptions but
        # they need to be fixed: attached is not the flag they should
        # look for.
        self.attached = True

    def _loadvdis(self):
        if self.cmd not in NEEDS_LOADVDIS:
            return 0
        if self.vdis:
            return

        self._init_hbadict()
        count = 0
        self.physical_size = 0
        root_dev_id = util.getrootdevID()

        xapi_session = self.session.xenapi
        known_scsid = {} # dict of ids processed within the following loop

        for key in self.hbadict.iterkeys():

            # We need a fresh sm_config everytime because it is modified
            # inside this loop
            sm_config = xapi_session.SR.get_sm_config(self.sr_ref)

            # The way we create vdi_path and the following check are
            # not clear at all
            vdi_path = os.path.join("/dev",key)
            if not self.devs.has_key(vdi_path):
                continue

            scsi_id = scsiutil.getSCSIid(vdi_path)
            if scsi_id == root_dev_id:
                util.SMlog("Skipping root device %s" %scsi_id)
                continue

            # Avoid false positives: this SR can already contain this
            # SCSIid during scan.
            scsi_key = "scsi-" + scsi_id
            if sm_config.has_key(scsi_key):
                # if we know about this scsid we can skip this specific dev
                if known_scsid.has_key(scsi_key):
                    util.SMlog("This SCSI id (%s) is already added" %scsi_id)
                    continue
                else:
                    # marked as known to avoid adding it again to sm_config
                    known_scsid[scsi_key] = ""
            elif util.test_SCSIid(self.session, None, scsi_id):
                util.SMlog("This SCSI id (%s) is used by another SR" %scsi_id)
                continue

            # getuniqueserial invokes again getSCSIid -> Fix!
            uuid = scsiutil.gen_uuid_from_string(
                scsiutil.getuniqueserial(vdi_path)
                )
            # We could have checked the SCSIid but the dictionary has
            # uuid as key.
            # We have already checked known_scsid, though. This block is
            # supposed to be always False
            if self.vdis.has_key(uuid):
                util.SMlog("Warning: unexpected code block reached with"
                           " uuid = %s" %scsi_id)
                continue

            obj = self.vdi(uuid)
            path = self.mpathmodule.path(scsi_id)
            ids = self.devs[vdi_path]
            obj._query(vdi_path, ids[4], uuid, scsi_id)
            self.vdis[uuid] = obj
            self.physical_size += obj.size

            count += 1

            # If we know about it no need to add to sm_config
            if known_scsid.has_key(scsi_key):
                continue

            # Prepare multipathing and make the other SRs know this SCSIid
            # is reserved.
            # Its counterpart is vdi_delete
            try:
                xapi_session.SR.add_to_sm_config(self.sr_ref, scsi_key, uuid)
                known_scsid[scsi_key] = ""
            except:
                util.SMlog("Warning: add_to_sm_config failed unexpectedly")

        return count


    def scan(self, sr_uuid):
        """
        This function is almost a copy of its base class equivalent.
        Main differences are:
        - Fixed erroneous size calculation
        - Set VDI names automatically
        - Avoid ScanRecord sync for missing VDIS (stale LUNs)

        The last one is called in the sub-sub class so we cannot simply
        extend the base class funcion but we need to override it
        """

        self._init_hbadict()
        if not self.passthrough:
            if not self.attached:
                raise xs_errors.XenError('SRUnavailable')

            self._loadvdis()
            
        # This block is almost SR.scan but without missing sync
        self._db_update()
        scanrecord = SR.ScanRecord(self)
        scanrecord.synchronise_new()
        scanrecord.synchronise_existing()


        # Fixing sizes calculation
        phys_util = 0
        for key in self.vdis:
            vdi_ref = self.session.xenapi.VDI.get_by_uuid(key)
            if B_util.is_vdi_attached(self.session, vdi_ref):
                phys_util += self.vdis[key].size
        self._set_stats(phys_util=phys_util)

        self._set_vdis_name()
        

    def _set_vdis_name(self):
        if not self.vdis:
            return
        for vdi_ref in self.session.xenapi.SR.get_VDIs(self.sr_ref):
            vdi_uuid = self.session.xenapi.VDI.get_uuid(vdi_ref)
            try:
                vdi = self.vdis[vdi_uuid]
            except:
                util.SMlog("Cannot set name for for %s" %vdi_uuid)
                continue
            self.session.xenapi.VDI.set_name_label(vdi_ref, vdi.SCSIid)


    def vdi(self, uuid):
        return RawHBAVDI(self, uuid)

    def _get_stats(self):
        stats = self.get_stats()
        self.physical_size = stats['physical_size']
        self.physical_utilisation = stats['physical_utilisation']
        self.virtual_allocation = stats['virtual_allocation']

    def get_stats(self):
        stats = {}
        xapi_session = self.session.xenapi
        sr_ref = xapi_session.SR.get_by_uuid(self.uuid)
        stats['physical_size'] = int(xapi_session.SR.get_physical_size(sr_ref))
        stats['physical_utilisation'] = int(xapi_session.SR.
                                            get_physical_utilisation(sr_ref))
        stats['virtual_allocation'] = int(xapi_session.SR.
                                          get_virtual_allocation(sr_ref))
        return stats

    def _set_stats(self, phys_size=None, phys_util=None):
        if phys_size != None:
            self.physical_size = phys_size
        if phys_util != None:
            self.physical_utilisation = phys_util
            self.virtual_allocation = phys_util
        self._db_update()

    def update_stats(self, phys_util):
        new_util = self.physical_utilisation + phys_util
        self._set_stats(phys_util=new_util)


    def _add_pbd_other_config(self, key, value):
        try:
            pbd_ref = util.find_my_pbd(self.session, self.host_ref, self.sr_ref)
        except:
            util.SMlog("No pbd for sr_ref %s on host_ref %s"
                       %(self.sr_ref, self.host_ref))
            return
        try:
            self.session.xenapi.PBD.add_to_other_config(pbd_ref, key, value)
        except:
            util.SMlog("add_to_other_config failed")


    def attach(self, sr_uuid):
        super(RawHBASR, self).attach(sr_uuid)
        if self.mpath == 'true':
            self._add_pbd_other_config('multipathed', 'true')


    def _reset_pbd_other_config(self):
        try:
            pbd_ref = util.find_my_pbd(self.session, self.host_ref, self.sr_ref)
        except:
            util.SMlog("No pbd for sr_ref %s on host_ref %s"
                       %(self.sr_ref, self.host_ref))
        for key in ["multipathed"]:
            try:
                self.session.xenapi.PBD.remove_from_other_config(pbd_ref, key)
            except:
                util.SMlog("remove_from_other_config failed")


    def detach(self, sr_uuid):
        """ Override base class function because we rely on xapi
        to check if VDIs are in use.
        Take care of removing multipath flags
        """
        self._reset_pbd_other_config()


class RawHBAVDI(LUNperVDI.RAWVDI):
    """Customized LUN-per-VDI class fro RawHBA storage repository"""

    def load(self, vdi_uuid):
        super(RawHBAVDI, self).load(vdi_uuid)
        self.managed = True

    # _query can work in two ways, if scsi_id is provided when called, the
    # fn gets all devices corresponding to the scsi_id and does a device rescan,
    # else it just rescan the device being passed.
    def _query(self, path, id, uuid=None, scsi_id=None):
        """Overloaded function with mostly duplicated code"""
        if uuid:
            self.uuid = uuid
        else:
            util.SMlog("RawHBA: uuid should not be generated..")
            self.uuid = scsiutil.gen_uuid_from_string(
                scsiutil.getuniqueserial(path)
                )
        if scsi_id:
            self.SCSIid = scsi_id
        else:
            # It is usually unnecessary to calculate it again but scsi_id
            # is used as a flag in this function and we cannot guarantee
            # this info is already available at call time
            self.SCSIid = scsiutil.getSCSIid(path)
        self.location = self.uuid
        self.vendor = scsiutil.getmanufacturer(path)
        self.serial = scsiutil.getserial(path)
        self.LUNid = id

        # Handle resize done at the array size. The resize gets reflected
        # only when the vdi is not in detached state. Do this if we the vdi
        # is known to xapi
        try:
            vdi_ref = self.sr.session.xenapi.VDI.get_by_uuid(self.uuid)
            # Check if the vbd is not in attached state, do a LUN rescan
            # to reflect the array LUN
            dev = [path]
            if scsi_id:
                # We want all the devices with this scsi_id
                dev = scsiutil._genReverseSCSIidmap(scsi_id)
            if self.sr.srcmd.cmd == "vdi_attach":
                scsiutil.refreshdev(dev)
            elif not B_util.is_vdi_attached(self.sr.session, vdi_ref):
                scsiutil.refreshdev(dev)
        except:
            pass

        self.size = scsiutil.getsize(path)
        self.path = path
        sm_config = util.default(self, "sm_config", lambda: {})
        sm_config['LUNid'] = str(self.LUNid)
        sm_config['SCSIid'] = self.SCSIid
        # Make sure to use kernel blkback (not blktap3) for raw LUNs
        sm_config['backend-kind'] = 'vbd'
        self.sm_config = sm_config

    def attach(self, sr_uuid, vdi_uuid):
        # Perform a device scan for all paths, to check for any LUN resize
        scsi_id = self.sm_config['SCSIid']
        devices = scsiutil._genReverseSCSIidmap(scsi_id)
        xapi_session = self.session.xenapi

        # At the vdi attach stage if devices are not found against the scsi_id, 
        # the two reasons would be 1. The machine is slave on which a bus scan
        # was not performed, perform a bus scan to rectify the same. 2. Genuine
        # HBA bus error, throw an error back.
        if len(devices) == 0:
            devscan.adapters()
            devices = scsiutil._genReverseSCSIidmap(scsi_id)

        # If no devices are found after a bus rescan, flag an error
        if len(devices) == 0:
            raise xs_errors.XenError('InvalidDev', \
                  opterr=('No HBA Device detected with SCSI Id[%s]') % scsi_id)

        # Run a query on devices against the scsi id to refresh the size
        dev_lun_info = scsiutil.cacheSCSIidentifiers()
        for dev in devices:
            self._query(dev, dev_lun_info[dev][4], vdi_uuid)

        #Update xapi with the new size
        vdi_ref = self.sr.session.xenapi.VDI.get_by_uuid(vdi_uuid)
        self.sr.session.xenapi.VDI.set_virtual_size(vdi_ref, str(self.size))

        # Multipath enable
        if self.sr.mpath == "true":
            self.sr.mpathmodule.refresh(scsi_id, len(devices))
            self.path = self.sr.mpathmodule.path(scsi_id)
            # The SCSIid is already stored inside SR sm_config.
            # We need only to trigger mpathcount
            try:
                cmd = ['/opt/xensource/sm/mpathcount.py', scsi_id]
                util.pread2(cmd)
            except:
                util.SMlog("RawHBA: something wrong with mpathcount")

        ret = VDI.VDI.attach(self, sr_uuid, vdi_uuid)
        self.sr.update_stats(self.size)
        return ret


    def delete(self, sr_uuid, vdi_uuid):
        util.SMlog("Raw LUN VDI delete")
        scsi_id = self.sm_config['SCSIid']
        xapi_session = self.session.xenapi

        # Cleaning up SR sm_config
        scsi_key = "scsi-" + scsi_id
        xapi_session.SR.remove_from_sm_config(self.sr.sr_ref, scsi_key)


    def detach(self, sr_uuid, vdi_uuid):
        scsi_id = self.sm_config['SCSIid']
        xapi_session = self.session.xenapi

        # Multipath disable
        if self.sr.mpath == "true":
            #devices = scsiutil._genReverseSCSIidmap(scsi_id)
            self.sr.mpathmodule.reset(scsi_id, True)
            util.remove_mpathcount_field(self.sr.session, self.sr.host_ref,
                                         self.sr.sr_ref, scsi_id)

        # Get size from xapi db
        vdi_ref = xapi_session.VDI.get_by_uuid(vdi_uuid)
        size = int(xapi_session.VDI.get_virtual_size(vdi_ref))

        self.sr.update_stats(-size)


if __name__ == '__main__':
    SRCommand.run(RawHBASR, DRIVER_INFO)
else:
    SR.registerSR(RawHBASR)
