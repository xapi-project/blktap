#!/usr/bin/env python
# Copyright (c) 2005-2007 XenSource, Inc. All use and distribution of this
# copyrighted material is governed by and subject to terms and conditions
# as licensed by XenSource, Inc. All other rights reserved.
# Xen, XenSource and XenEnterprise are either registered trademarks or
# trademarks of XenSource Inc. in the United States and/or other countries.
#
#
# FileSR: local-file storage repository

import SR, VDI, SRCommand, FileSR, util
import errno
import os, re
import xs_errors

CAPABILITIES = ["VDI_CREATE","VDI_DELETE","VDI_ATTACH","VDI_DETACH", \
                "VDI_CLONE","VDI_SNAPSHOT","VDI_LOCK","VDI_UNLOCK"]

CONFIGURATION = [ [ 'server', 'hostname or IP address of NFS server (required)' ], \
                  [ 'serverpath', 'path on remote server (required)' ] ]

                  
DRIVER_INFO = {
    'name': 'NFS VHD',
    'description': 'SR plugin which stores disks as VHD files on a remote NFS filesystem',
    'vendor': 'XenSource Inc',
    'copyright': '(C) 2007 XenSource Inc',
    'driver_version': '0.1',
    'required_api_version': '0.1',
    'capabilities': CAPABILITIES,
    'configuration': CONFIGURATION
    }

# The algorithm for tcp and udp (at least in the linux kernel) for 
# NFS timeout on softmounts is as follows:
#
# UDP:
# As long as the request wasn't started more than timeo * (2 ^ retrans) 
# in the past, keep doubling the timeout.
#
# TCP:
# As long as the request wasn't started more than timeo * (1 + retrans) 
# in the past, keep increaing the timeout by timeo.
#
# The time when the retrans may retry has been made will be:
# For udp: timeo * (2 ^ retrans * 2 - 1)
# For tcp: timeo * n! where n is the smallest n for which n! > 1 + retrans
#
# thus for retrans=1, timeo can be the same for both tcp and udp, 
# because the first doubling (timeo*2) is the same as the first increment
# (timeo+timeo).

# timeouts in seconds 
SOFTMOUNT_TIMEOUT  = 2.0
LOCK_LEASE_TIMEOUT = 30.0
LOCK_LEASE_GRACE = 5

class NFSSR(FileSR.FileSR):
    """Local file storage repository"""
    def handles(type):
        return type == 'nfs'
    handles = staticmethod(handles)

    def load(self, sr_uuid):
        self.sr_vditype = SR.DEFAULT_TAP
        if not self.dconf.has_key('serverpath'):
            raise xs_errors.XenError('ConfigServerPathMissing')
        if not self.dconf.has_key('server'):
            raise xs_errors.XenError('ConfigServerMissing')
        if not self._isvalidpathstring(self.dconf['serverpath']):
            raise xs_errors.XenError('ConfigServerPathBad', \
                  opterr='serverpath is %s' % self.dconf['serverpath'])

        self.remotepath = os.path.join(self.dconf['serverpath'], sr_uuid)

        self.remoteserver = self.dconf['server']
        self.path = os.path.join(SR.MOUNT_BASE, sr_uuid)

    def attach(self, sr_uuid):
        if not util.ioretry(lambda: self._checkmount()):
            try:
                # make sure NFS over TCP/IP V3 is supported on the server
                util.ioretry(lambda: util.pread(["/usr/sbin/rpcinfo","-t", \
                              "%s" % self.remoteserver, "nfs","3"]), \
                              errlist=[errno.EPERM], nofail=1)
            except util.CommandException, inst:
                raise xs_errors.XenError('NFSVersion', \
                      opterr='or NFS server timed out')
            try:
                # make a mountpoint:
                if not util.ioretry(lambda: util.isdir(self.path)):
                    util.ioretry(lambda: util.makedirs(self.path))

                timeout = int((SOFTMOUNT_TIMEOUT / 3.0) * 10.0)
                util.ioretry(lambda: util.pread(["mount.nfs", "%s:%s" \
                             % (self.remoteserver, self.remotepath),  \
                             self.path, "-o", \
                             "udp,soft,timeo=%d,retrans=1,noac" % \
                             timeout]), errlist=[errno.EPIPE, errno.EIO],
                             nofail=1)
            except util.CommandException, inst:
                raise xs_errors.XenError('NFSMount')
        return super(NFSSR, self).attach(sr_uuid)
        
    def detach(self, sr_uuid):
        if not util.ioretry(lambda: self._checkmount()):
            return
        try:
            util.pread(["umount", self.path])
            os.rmdir(self.path)
        except util.CommandException, inst:
            raise xs_errors.XenError('NFSUnMount', \
                  opterr='error is %d' % inst.code)
        except OSError, inst:
            raise xs_errors.XenError('NFSUnMount', \
                  opterr='directory removal error is %d' % inst.sterror)
        return super(NFSSR, self).detach(sr_uuid)
        
    def create(self, sr_uuid, size):
        if util.ioretry(lambda: self._checkmount()):
            raise xs_errors.XenError('NFSAttached')

        # Set the target path temporarily to the base dir
        # so that we can create the target SR directory
        self.remotepath = self.dconf['serverpath']
        try:
            self.attach(sr_uuid)
        except:
            try:
                os.rmdir(self.path)
            except:
                pass
            raise xs_errors.XenError('NFSMount')
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
                    raise xs_errors.XenError('NFSCreate', \
                          opterr='remote directory creation error is %d' \
                          % inst.code)
        self.detach(sr_uuid)

    def delete(self, sr_uuid):
        # try to remove/delete non VDI contents first
        super(NFSSR, self).delete(sr_uuid)
        try:
            if util.ioretry(lambda: self._checkmount()):
                self.detach(sr_uuid)

            # Set the target path temporarily to the base dir
            # so that we can remove the target SR directory
            self.remotepath = self.dconf['serverpath']
            self.attach(sr_uuid)
            newpath = os.path.join(self.path, sr_uuid)

            if util.ioretry(lambda: util.pathexists(newpath)):
                util.ioretry(lambda: os.rmdir(newpath))
            self.detach(sr_uuid)
        except util.CommandException, inst:
            self.detach(sr_uuid)
            if inst.code != errno.ENOENT:
                raise xs_errors.XenError('NFSDelete')

    def vdi(self, uuid):
        return NFSVDI(self, uuid)
    
    def _checkmount(self):
        return util.pathexists(self.path) \
               and util.ismount(self.path)

class NFSVDI(FileSR.FileVDI):
    def load(self, vdi_uuid):
        super(NFSVDI, self).load(vdi_uuid)
        self.lockable = True
        self.locked = util.ioretry(lambda: self._getlockstatus())

    def lock(self, sr_uuid, vdi_uuid, force, l_uuid):
        util.ioretry(lambda: self._lockt(force, l_uuid))
        if self.status < 0:
            oktosteal = util.ioretry(lambda: self._checklock(l_uuid))
            if oktosteal:
                util.ioretry(lambda: self._lockt("1", l_uuid))
        return super(NFSVDI, self).lock(sr_uuid, vdi_uuid, force, l_uuid)

    def _checklock(self, l_uuid):
        cmd = [SR.LOCK_UTIL, "delta", self.path]
        out = util.pread2(cmd)
        time = int(out.split()[0])
        limit = int(out.split()[1])
        if time < 0:
            return 0
        if time > limit + LOCK_LEASE_GRACE:
            return 1
        return 0

    def _lockt(self, force, l_uuid):
        cmd = [SR.LOCK_UTIL, "lock", self.path, "w", force, l_uuid, \
               str(int(LOCK_LEASE_TIMEOUT))]
        out = util.pread2(cmd)
        self.status = int(out.split()[0])
        self.leasetime = int(out.split()[1])
        if self.status >= 0:
            self.xenstore_keys = l_uuid + ':w'
            self.locked = True

    def delete(self, sr_uuid, vdi_uuid):
        # check for NFS locks before removing vdi
        if self.locked:
            out = util.ioretry(lambda: self._lockd())
            
            status = int(out.split()[0])
            lease = int(out.split()[1])
            if status >= 0:
                if status > lease + LOCK_LEASE_GRACE:
                    # wkcfix: need to remove lock file
                    pass
                elif status > lease:
                    raise xs_errors.XenError('VDIInUse', \
                          opterr='VDI in lock grace period')
                else:
                    raise xs_errors.XenError('VDIInUse', \
                          opterr='VDI still locked')
        super(NFSVDI, self).delete(sr_uuid, vdi_uuid)

    def _lockd(self):
        cmd = [SR.LOCK_UTIL, "delta", self.path]
        out = util.pread2(cmd)
        return out

    def unlock(self, sr_uuid, vdi_uuid, l_uuid):
        try:
            cmd = [SR.LOCK_UTIL, "unlock", self.path, "w", l_uuid]
            self.status = util.ioretry(lambda: util.pread2(cmd))
        except util.CommandException, inst:
            if inst.code != errno.ENOENT:
                raise xs_errors.XenError('VDIInUse', \
                          opterr='Unable to release lock')
        return super(NFSVDI, self).unlock(sr_uuid, vdi_uuid, l_uuid)

    def _getlockstatus(self):
        if util.ioretry(lambda: util.pathexists(self.sr.path)):
            if len(filter(self.match_locks, util.ioretry(lambda: \
                               util.listdir(self.sr.path)))) > 0:
                return True
        return False

    def match_locks(self, s):
        regex = re.compile("%s.%s.xenlk" % (self.uuid, self.sr.sr_vditype))
        return regex.search(s, 0)
                        
if __name__ == '__main__':
    SRCommand.run(NFSSR, DRIVER_INFO)
else:
    SR.registerSR(NFSSR)
