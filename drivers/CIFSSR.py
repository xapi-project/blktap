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
# CIFSSR: CIFS filesystem based storage repository

import SR, VDI, SRCommand, FileSR, util
import errno
import os, re, sys
import xml.dom.minidom
import xmlrpclib
import xs_errors
import vhdutil
from lock import Lock
import cleanup

CAPABILITIES = ["SR_PROBE","SR_UPDATE", "SR_CACHING",
                "VDI_CREATE","VDI_DELETE","VDI_ATTACH","VDI_DETACH",
                "VDI_UPDATE", "VDI_CLONE","VDI_SNAPSHOT","VDI_RESIZE",
                "VDI_GENERATE_CONFIG",
                "VDI_RESET_ON_BOOT/2", "ATOMIC_PAUSE"]

CONFIGURATION = [ [ 'server', 'Full path to share root on CIFS server (required)' ], \
                  [ 'username', 'The username to be used during CIFS authentication' ], \
                  [ 'password', 'The password to be used during CIFS authentication' ] ]

DRIVER_INFO = {
    'name': 'CIFS VHD',
    'description': 'SR plugin which stores disks as VHD files on a remote CIFS filesystem',
    'vendor': 'Citrix Systems Inc',
    'copyright': '(C) 2015 Citrix Systems Inc',
    'driver_version': '1.0',
    'required_api_version': '1.0',
    'capabilities': CAPABILITIES,
    'configuration': CONFIGURATION
    }

DRIVER_CONFIG = {"ATTACH_FROM_CONFIG_WITH_TAPDISK": True}

# The mountpoint for the directory when performing an sr_probe.  All probes
# are guaranteed to be serialised by xapi, so this single mountpoint is fine.
PROBE_MOUNTPOINT = os.path.join(SR.MOUNT_BASE, "probe")

class CifsException(Exception):
    def __init__(self, errstr):
        self.errstr = errstr

# server = //cifs-server/vol1 - ie the export path on the CIFS server 
# mountpoint = /var/run/sr-mount/CIFS/<cifs_server_name>/<share_name>/uuid
# linkpath = mountpoint/uuid - path to SR directory on share
# path = /var/run/sr-mount/uuid - symlink to SR directory on share
class CIFSSR(FileSR.FileSR):
    """CIFS file-based storage repository"""
    def handles(type):
        return type == 'cifs'
    handles = staticmethod(handles)

    def load(self, sr_uuid):
        self.ops_exclusive = FileSR.OPS_EXCLUSIVE
        self.lock = Lock(vhdutil.LOCK_TYPE_SR, self.uuid)
        self.sr_vditype = SR.DEFAULT_TAP
        self.driver_config = DRIVER_CONFIG
        if not self.dconf.has_key('server'):
            raise xs_errors.XenError('ConfigServerMissing')
        self.remoteserver = self.dconf['server']
        if self.sr_ref and self.session is not None :
            self.sm_config = self.session.xenapi.SR.get_sm_config(self.sr_ref)
        else:
            self.sm_config = self.srcmd.params.get('sr_sm_config') or {}
        self.credentials = None
        self.mountpoint = os.path.join(SR.MOUNT_BASE, 'CIFS', self.__extract_server(), sr_uuid)
        self.linkpath = os.path.join(self.mountpoint, 
                                           sr_uuid or "")
        # Remotepath is the absolute path inside a share that is to be mounted
        # For a CIFS SR, only the root can be mounted.
        self.remotepath = ''
        self.path = os.path.join(SR.MOUNT_BASE, sr_uuid)
        self._check_o_direct()

    def checkmount(self):
        return util.ioretry(lambda: ((util.pathexists(self.mountpoint) and \
				util.ismount(self.mountpoint)) and \
                                util.pathexists(self.linkpath)))

    def mount(self, mountpoint=None):
        """Mount the remote CIFS export at 'mountpoint'"""
        if mountpoint == None:
            mountpoint = self.mountpoint
        elif not util.is_string(mountpoint) or mountpoint == "":
            raise CifsException("mountpoint not a string object")

        missing_params = set()

        if not self.dconf.has_key('username'):
            missing_params.add('username')

        if not (self.dconf.has_key('password') or
                self.dconf.has_key('password_secret')):
            missing_params.add('password')

        if missing_params:
            errstr = 'device-config is missing the following parameters: ' + \
                     ', '.join([param for param in missing_params])
            raise xs_errors.XenError('ConfigParamsMissing', opterr=errstr)

        try:
            if not util.ioretry(lambda: util.isdir(mountpoint)):
                util.ioretry(lambda: util.makedirs(mountpoint))
        except util.CommandException, inst:
            raise CifsException("Failed to make directory: code is %d" %
                                inst.code)

        self.credentials = os.path.join("/tmp", util.gen_uuid())

        options = ','.join([
                'sec=ntlm',
                'cache=none',
                'vers=3.0',
                'credentials=%s' % self.credentials
        ])

        dom_username = self.dconf['username'].split('\\')

        if len(dom_username) == 1:
            domain = None
            username = dom_username[0]
        elif len(dom_username) == 2:
            domain = dom_username[0]
            username = dom_username[1]
        else:
            raise CifsException("A maximum of 2 tokens are expected "
                                "(<domain>\<username>). {} were given."
                                .format(len(dom_username)))

        domain = util.to_plain_string(domain)
        username = util.to_plain_string(username)

        if self.dconf.has_key('password_secret'):
            password = util.get_secret(
                    self.session,
                    self.dconf['password_secret']
            )
        else:
            password = self.dconf['password']

        password = util.to_plain_string(password)

        cred_str = 'username={}\npassword={}\n'.format(username, password)

        if domain:
            cred_str += 'domain={}\n'.format(domain)

        # Open credentials file and truncate
        try:
            with open(self.credentials, 'w') as f:
                f.write(cred_str)
        except IOError, e:
            raise CifsException("Failed to create credentials file")

        try:
            util.ioretry(lambda:
                util.pread(["mount.cifs", self.remoteserver,
                mountpoint, "-o", options]),
                errlist=[errno.EPIPE, errno.EIO],
                maxretry=2, nofail=True)
        except util.CommandException, inst:
            raise CifsException("mount failed with return code %d" % inst.code)
        finally:
            try:
                os.unlink(self.credentials)
            except OSError:
                util.SMlog("Error when trying to delete "
                           "credentials files /tmp/<uuid>")

        # Sanity check to ensure that the user has at least RO access to the
        # mounted share. Windows sharing and security settings can be tricky.
        try:
            util.listdir(mountpoint)
        except util.CommandException:
            try:
                self.unmount(mountpoint, True)
            except CifsException:
                util.logException('CIFSSR.unmount()')
            raise CifsException("Permission denied. "
                                "Please check user privileges.")
 
    def unmount(self, mountpoint, rmmountpoint):
        """Unmount the remote CIFS export at 'mountpoint'"""
        try:
            util.pread(["umount", mountpoint])
        except util.CommandException, inst:
            raise CifsException("umount failed with return code %d" % inst.code)

        if rmmountpoint:
            try:
                os.rmdir(mountpoint)
            except OSError, inst:
                raise CifsException("rmdir failed with error '%s'" % inst.strerror)

    def __extract_server(self):
        return self.remoteserver[2:]

    def __check_license(self):
        """Raises an exception if CIFS is not licensed."""
        if self.session is None or (isinstance(self.session, str) and \
                self.session == ""):
            raise xs_errors.XenError('NoCifsLicense',
                    'No session object to talk to XAPI')
        restrictions = util.get_pool_restrictions(self.session)
        if 'restrict_cifs' in restrictions and \
                restrictions['restrict_cifs'] == "true":
            raise xs_errors.XenError('NoCifsLicense')

    def attach(self, sr_uuid):
        if not self.checkmount():
            try: 
                self.mount()
                os.symlink(self.linkpath, self.path)
            except CifsException, exc:
                raise xs_errors.XenError('CIFSMount', opterr=exc.errstr)
        self.attached = True

    def probe(self):
        try:
            err = "CIFSMount"
            self.mount(PROBE_MOUNTPOINT)
            sr_list = filter(util.match_uuid, util.listdir(PROBE_MOUNTPOINT))
            err = "CIFSUnMount"
            self.unmount(PROBE_MOUNTPOINT, True)
        except CifsException, inst:
            raise xs_errors.XenError(err, opterr=inst.errstr)
        except (util.CommandException, xs_errors.XenError):
            raise

        # Create a dictionary from the SR uuids to feed SRtoXML()
        sr_dict = {sr_uuid : {} for sr_uuid in sr_list}

        return util.SRtoXML(sr_dict)

    def detach(self, sr_uuid):
        """Detach the SR: Unmounts and removes the mountpoint"""
        if not self.checkmount():
            return
        util.SMlog("Aborting GC/coalesce")
        cleanup.abort(self.uuid)

        # Change directory to avoid unmount conflicts
        os.chdir(SR.MOUNT_BASE)

        try:
            self.unmount(self.mountpoint, True)
            os.unlink(self.path)
        except CifsException, exc:
            raise xs_errors.XenError('CIFSUnMount', opterr=exc.errstr)

        self.attached = False
        
    def create(self, sr_uuid, size):
        self.__check_license()

        if self.checkmount():
            raise xs_errors.XenError('CIFSAttached')

        try:
            self.mount()
        except CifsException, exc:
            try:
                os.rmdir(self.mountpoint)
            except:
                pass
            raise xs_errors.XenError('CIFSMount', opterr=exc.errstr)

        if util.ioretry(lambda: util.pathexists(self.linkpath)):
            if len(util.ioretry(lambda: util.listdir(self.linkpath))) != 0:
                self.detach(sr_uuid)
                raise xs_errors.XenError('SRExists')
        else:
            try:
                util.ioretry(lambda: util.makedirs(self.linkpath))
                os.symlink(self.linkpath, self.path)
            except util.CommandException, inst:
                if inst.code != errno.EEXIST:
                    try:
                        self.unmount(self.mountpoint, True)
                    except CifsException:
                        util.logException('CIFSSR.unmount()')
                    raise xs_errors.XenError(
                            'CIFSCreate',
                            opterr="remote directory creation error: {}"
                                    .format(os.strerror(inst.code))
                    )
        self.detach(sr_uuid)

    def delete(self, sr_uuid):
        # try to remove/delete non VDI contents first
        super(CIFSSR, self).delete(sr_uuid)
        try:
            if self.checkmount():
                self.detach(sr_uuid)

            self.mount()
            if util.ioretry(lambda: util.pathexists(self.linkpath)):
                util.ioretry(lambda: os.rmdir(self.linkpath))
            self.unmount(self.mountpoint, True)
        except util.CommandException, inst:
            self.detach(sr_uuid)
            if inst.code != errno.ENOENT:
                raise xs_errors.XenError('CIFSDelete')

    def vdi(self, uuid, loadLocked = False):
        if not loadLocked:
            return CIFSFileVDI(self, uuid)
        return CIFSFileVDI(self, uuid)
    
class CIFSFileVDI(FileSR.FileVDI):
    def attach(self, sr_uuid, vdi_uuid):
        if not hasattr(self,'xenstore_data'):
            self.xenstore_data = {}
            
        self.xenstore_data["storage-type"]="cifs"

        return super(CIFSFileVDI, self).attach(sr_uuid, vdi_uuid)

    def generate_config(self, sr_uuid, vdi_uuid):
        util.SMlog("CIFSFileVDI.generate_config")
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
        util.SMlog("CIFSFileVDI.attach_from_config")
        try:
            if not util.pathexists(self.sr.path):
                self.sr.attach(sr_uuid)
        except:
            util.logException("CIFSFileVDI.attach_from_config")
            raise xs_errors.XenError('SRUnavailable', \
                        opterr='Unable to attach from config')


if __name__ == '__main__':
    SRCommand.run(CIFSSR, DRIVER_INFO)
else:
    SR.registerSR(CIFSSR)
#
