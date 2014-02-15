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
# FileSR: local-file storage repository

import SR, VDI, SRCommand, FileSR, util
import errno
import os, re, sys
import xml.dom.minidom
import xmlrpclib
import xs_errors
import nfs
import vhdutil
from lock import Lock
import cleanup
import XenAPI

CAPABILITIES = ["SR_PROBE","SR_UPDATE", "SR_CACHING",
                "VDI_CREATE","VDI_DELETE","VDI_ATTACH","VDI_DETACH",
                "VDI_UPDATE", "VDI_CLONE","VDI_SNAPSHOT","VDI_RESIZE",
                "VDI_GENERATE_CONFIG",
                "VDI_RESET_ON_BOOT/2", "ATOMIC_PAUSE"]

CONFIGURATION = [ [ 'server', 'hostname or IP address of NFS server (required)' ], \
                  [ 'serverpath', 'path on remote server (required)' ] ]

                  
DRIVER_INFO = {
    'name': 'NFS VHD',
    'description': 'SR plugin which stores disks as VHD files on a remote NFS filesystem',
    'vendor': 'Citrix Systems Inc',
    'copyright': '(C) 2008 Citrix Systems Inc',
    'driver_version': '1.0',
    'required_api_version': '1.0',
    'capabilities': CAPABILITIES,
    'configuration': CONFIGURATION
    }

DRIVER_CONFIG = {"ATTACH_FROM_CONFIG_WITH_TAPDISK": True}


# The mountpoint for the directory when performing an sr_probe.  All probes
# are guaranteed to be serialised by xapi, so this single mountpoint is fine.
PROBE_MOUNTPOINT = "probe"
NFSPORT = 2049
DEFAULT_TRANSPORT = "tcp"


class NFSSR(FileSR.FileSR):
    """NFS file-based storage repository"""
    def handles(type):
        return type == 'nfs'
    handles = staticmethod(handles)


    def load(self, sr_uuid):
        self.ops_exclusive = FileSR.OPS_EXCLUSIVE
        self.lock = Lock(vhdutil.LOCK_TYPE_SR, self.uuid)
        self.sr_vditype = SR.DEFAULT_TAP
        self.driver_config = DRIVER_CONFIG
        if not self.dconf.has_key('server'):
            raise xs_errors.XenError('ConfigServerMissing')
        self.remoteserver = self.dconf['server']
        self.nosubdir = False
        if self.sr_ref and self.session is not None :
            self.sm_config = self.session.xenapi.SR.get_sm_config(self.sr_ref)
        else:
            self.sm_config = self.srcmd.params.get('sr_sm_config') or {}
        self.nosubdir = self.sm_config.get('nosubdir') == "true"
        if self.dconf.has_key('serverpath'):
            self.remotepath = os.path.join(self.dconf['serverpath'],
                                           not self.nosubdir and sr_uuid or "")
        self.path = os.path.join(SR.MOUNT_BASE, sr_uuid)

        # Test for the optional 'nfsoptions' dconf attribute
        self.transport = DEFAULT_TRANSPORT
        if self.dconf.has_key('useUDP') and self.dconf['useUDP'] == 'true':
            self.transport = "udp"


    def validate_remotepath(self, scan):
        if not self.dconf.has_key('serverpath'):
            if scan:
                try:
                    self.scan_exports(self.dconf['server'])
                except:
                    pass
            raise xs_errors.XenError('ConfigServerPathMissing')
        if not self._isvalidpathstring(self.dconf['serverpath']):
            raise xs_errors.XenError('ConfigServerPathBad', \
                  opterr='serverpath is %s' % self.dconf['serverpath'])

    def check_server(self):
        try:
            nfs.check_server_tcp(self.remoteserver)
        except nfs.NfsException, exc:
            raise xs_errors.XenError('NFSVersion',
                                     opterr=exc.errstr)


    def mount(self, mountpoint, remotepath, timeout = 0):
        try:
            nfs.soft_mount(mountpoint, self.remoteserver, remotepath,
                    self.transport, timeout)
        except nfs.NfsException, exc:
            raise xs_errors.XenError('NFSMount', opterr=exc.errstr)


    def attach(self, sr_uuid):
        if not self._checkmount():
            self.validate_remotepath(False)
            util._testHost(self.dconf['server'], NFSPORT, 'NFSTarget')
            io_timeout = util.get_nfs_timeout(self.session, sr_uuid)
            self.mount_remotepath(sr_uuid, io_timeout)
        self.attached = True


    def mount_remotepath(self, sr_uuid, timeout = 0):
        if not self._checkmount():
            self.check_server()
            self.mount(self.path, self.remotepath, timeout)


    def probe(self):
        # Verify NFS target and port
        util._testHost(self.dconf['server'], NFSPORT, 'NFSTarget')
        
        self.validate_remotepath(True)
        self.check_server()

        temppath = os.path.join(SR.MOUNT_BASE, PROBE_MOUNTPOINT)

        self.mount(temppath, self.remotepath)
        try:
            return nfs.scan_srlist(temppath)
        finally:
            try:
                nfs.unmount(temppath, True)
            except:
                pass


    def detach(self, sr_uuid):
        """Detach the SR: Unmounts and removes the mountpoint"""
        if not self._checkmount():
            return
        util.SMlog("Aborting GC/coalesce")
        cleanup.abort(self.uuid)

        # Change directory to avoid unmount conflicts
        os.chdir(SR.MOUNT_BASE)

        try:
            nfs.unmount(self.path, True)
        except nfs.NfsException, exc:
            raise xs_errors.XenError('NFSUnMount', opterr=exc.errstr)

        self.attached = False
        

    def create(self, sr_uuid, size):
        util._testHost(self.dconf['server'], NFSPORT, 'NFSTarget')
        self.validate_remotepath(True)
        if self._checkmount():
            raise xs_errors.XenError('NFSAttached')

        # Set the target path temporarily to the base dir
        # so that we can create the target SR directory
        self.remotepath = self.dconf['serverpath']
        try:
            self.mount_remotepath(sr_uuid)
        except Exception, exn:
            try:
                os.rmdir(self.path)
            except:
                pass
            raise exn

        if not self.nosubdir:
            newpath = os.path.join(self.path, sr_uuid)
            if util.ioretry(lambda: util.pathexists(newpath)):
                if len(util.ioretry(lambda: util.listdir(newpath))) != 0:
                    self.detach(sr_uuid)
                    raise xs_errors.XenError('SRExists')
            else:
                try:
                    util.ioretry(lambda: util.makedirs(newpath))
                except util.CommandException, inst:
                    if inst.code != errno.EEXIST:
                        self.detach(sr_uuid)
                        raise xs_errors.XenError('NFSCreate',
                            opterr='remote directory creation error is %d'
                            % inst.code)
        self.detach(sr_uuid)

    def delete(self, sr_uuid):
        # try to remove/delete non VDI contents first
        super(NFSSR, self).delete(sr_uuid)
        try:
            if self._checkmount():
                self.detach(sr_uuid)

            # Set the target path temporarily to the base dir
            # so that we can remove the target SR directory
            self.remotepath = self.dconf['serverpath']
            self.mount_remotepath(sr_uuid)
            if not self.nosubdir:
                newpath = os.path.join(self.path, sr_uuid)
                if util.ioretry(lambda: util.pathexists(newpath)):
                    util.ioretry(lambda: os.rmdir(newpath))
            self.detach(sr_uuid)
        except util.CommandException, inst:
            self.detach(sr_uuid)
            if inst.code != errno.ENOENT:
                raise xs_errors.XenError('NFSDelete')

    def vdi(self, uuid, loadLocked = False):
        if not loadLocked:
            return NFSFileVDI(self, uuid)
        return NFSFileVDI(self, uuid)
    
    def scan_exports(self, target):
        util.SMlog("scanning2 (target=%s)" % target)
        dom = nfs.scan_exports(target)
        print >>sys.stderr,dom.toprettyxml()

class NFSFileVDI(FileSR.FileVDI):
    def attach(self, sr_uuid, vdi_uuid):
        if not hasattr(self,'xenstore_data'):
            self.xenstore_data = {}
            
        self.xenstore_data["storage-type"]="nfs"

        return super(NFSFileVDI, self).attach(sr_uuid, vdi_uuid)

    def generate_config(self, sr_uuid, vdi_uuid):
        util.SMlog("NFSFileVDI.generate_config")
        if not util.pathexists(self.path):
                raise xs_errors.XenError('VDIUnavailable')
        resp = {}
        resp['device_config'] = self.sr.dconf
        resp['sr_uuid'] = sr_uuid
        resp['vdi_uuid'] = vdi_uuid
        resp['sr_sm_config'] = self.sr.sm_config
        resp['command'] = 'vdi_attach_from_config'
        # Return the 'config' encoded within a normal XMLRPC response so that
        # we can use the regular response/error parsing code.
        config = xmlrpclib.dumps(tuple([resp]), "vdi_attach_from_config")
        return xmlrpclib.dumps((config,), "", True)

    def attach_from_config(self, sr_uuid, vdi_uuid):
        """Used for HA State-file only. Will not just attach the VDI but
        also start a tapdisk on the file"""
        util.SMlog("NFSFileVDI.attach_from_config")
        try:
            self.sr.attach(sr_uuid)
        except:
            util.logException("NFSFileVDI.attach_from_config")
            raise xs_errors.XenError('SRUnavailable', \
                        opterr='Unable to attach from config')


if __name__ == '__main__':
    SRCommand.run(NFSSR, DRIVER_INFO)
else:
    SR.registerSR(NFSSR)
