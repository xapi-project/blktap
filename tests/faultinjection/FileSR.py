#!/usr/bin/env python
# Copyright (c) 2005-2007 XenSource, Inc. All use and distribution of this
# copyrighted material is governed by and subject to terms and conditions
# as licensed by XenSource, Inc. All other rights reserved.
# Xen, XenSource and XenEnterprise are either registered trademarks or
# trademarks of XenSource Inc. in the United States and/or other countries.
#
#
# FileSR: local-file storage repository

import SR, VDI, SRCommand, util
import statvfs
import os, re
import errno
import xml.dom.minidom
import xs_errors

geneology = {}
CAPABILITIES = ["VDI_CREATE","VDI_DELETE","VDI_ATTACH","VDI_DETACH", \
                "VDI_CLONE","VDI_SNAPSHOT","VDI_LOCK","VDI_UNLOCK", \
                "VDI_GET_PARAMS"]

CONFIGURATION = [ [ 'path', 'path where images are stored (required)' ] ]
                  
DRIVER_INFO = {
    'name': 'Local EXT3 VHD',
    'description': 'SR plugin which represents disks as VHD files stored on a local path',
    'vendor': 'XenSource Inc',
    'copyright': '(C) 2007 XenSource Inc',
    'driver_version': '0.1',
    'required_api_version': '0.1',
    'capabilities': CAPABILITIES,
    'configuration': CONFIGURATION
    }

ENFORCE_VIRT_ALLOC = False
MAX_DISK_MB = 2 * 1024 * 1024

class FileSR(SR.SR):
    """Local file storage repository"""
    def handles(srtype):
        return srtype == 'file'
    handles = staticmethod(handles)

    def load(self, sr_uuid):
        self.sr_vditype = SR.DEFAULT_TAP
        if not self.dconf.has_key('location') or  not self.dconf['location']:
            raise xs_errors.XenError('ConfigLocationMissing')
        self.path = self.dconf['location']
        self.attached = False

    def create(self, sr_uuid, size):
        """ Create the SR.  The path must not already exist, or if it does, 
        it must be empty.  (This accounts for the case where the user has
        mounted a device onto a directory manually and want to use this as the
        root of a file-based SR.) """
        try:
            if util.ioretry(lambda: util.pathexists(self.path)):
                if len(util.ioretry(lambda: util.listdir(self.path))) != 0:
                    raise xs_errors.XenError('SRExists')
            else:
                try:
                    util.ioretry(lambda: os.mkdir(self.path))
                except util.CommandException, inst:
                    if inst.code == errno.EEXIST:
                        raise xs_errors.XenError('SRExists')
                    else:
                        raise xs_errors.XenError('FileSRCreate', \
                              opterr='directory creation failure %d' \
                              % inst.code)
        except:
            raise xs_errors.XenError('FileSRCreate')

    def delete(self, sr_uuid):
        if not self._checkpath(self.path):
            raise xs_errors.XenError('SRUnavailable', \
                  opterr='no such directory %s' % self.path)

        # check to make sure no VDIs are present; then remove old 
        # files that are non VDI's
        try:
            if util.ioretry(lambda: util.pathexists(self.path)):
                #Load the VDI list
                self._loadvdis()
                self._cleanup_leafnodes()
                for uuid in self.vdis:
                    if not self.vdis[uuid].deleted:
                        raise xs_errors.XenError('SRNotEmpty', \
                              opterr='VDIs still exist in SR')

                # remove everything else, there are no vdi's
                for name in util.ioretry(lambda: util.listdir(self.path)):
                    fullpath =  os.path.join(self.path,name)
                    try:
                        util.ioretry(lambda: os.unlink(fullpath))
                    except util.CommandException, inst:
                        if inst.code != errno.ENOENT and \
                           inst.code != errno.EISDIR:
                            raise xs_errors.XenError('FileSRDelete', \
                                  opterr='failed to remove %s error %d' \
                                  % (fullpath, inst.code))
        except util.CommandException, inst:
            raise xs_errors.XenError('FileSRDelete', \
                  opterr='error %d' % inst.code)

    def attach(self, sr_uuid):
        if not self._checkpath(self.path):
            raise xs_errors.XenError('SRUnavailable', \
                  opterr='no such directory %s' % self.path)
        self.attached = True

    def detach(self, sr_uuid):
        self.attached = False

    def scan(self, sr_uuid):
        raise xs_errors.XenError('SRScan')
        if not self._checkpath(self.path):
            raise xs_errors.XenError('SRUnavailable', \
                  opterr='no such directory %s' % self.path)

        try:
            if not self.vdis:
                self._loadvdis()
            self._cleanup_leafnodes()
            if not self.passthrough:
                self.physical_size = self._getsize()
                self.physical_utilisation  = self._getutilisation()

            return super(FileSR, self).scan(sr_uuid)
        except:
            raise xs_errors.XenError('SRScan')

    def type(self, sr_uuid):
        return super(FileSR, self).type(sr_uuid)

    def vdi(self, uuid):
        return FileVDI(self, uuid)

    def added_vdi(self, vdi):
        self.vdis[vdi.uuid] = vdi

    def deleted_vdi(self, uuid):
        if uuid in self.vdis:
            del self.vdis[uuid]

    def replay(self, uuid):
        try:
            file = open(self.path + "/filelog.txt", "r")
            data = file.readlines()
            file.close()
            self._process_replay(data)
        except:
            raise xs_errors.XenError('SRLog')

    def _checkpath(self, path):
        try:
            if util.ioretry(lambda: util.pathexists(path)):
                if util.ioretry(lambda: util.isdir(path)):
                    return True
            return False
        except util.CommandException, inst:
            raise xs_errors.XenError('EIO', \
                  opterr='IO error checking path %s' % path)

    def _loadvdis(self):
        if self.vdis:
            return

        for name in filter(util.match_uuid, util.ioretry( \
                          lambda: util.listdir(self.path))):
            fields = name.split('.')
            if len(fields) == 2 and fields[1] == self.sr_vditype:
                fullpath = os.path.join(self.path,name)
                uuid = fields[0]
                try:
                    self.vdis[uuid] = self.vdi(uuid)
                    self.vdis[uuid].marked_deleted = self.vdis[uuid].hidden
                except SR.SRException, inst:
                    pass

        # Mark parent VDIs as Read-only and generate virtual allocation
        self.virtual_allocation = 0
        for uuid, vdi in self.vdis.iteritems():
            if vdi.parent:
                if self.vdis.has_key(vdi.parent):
                    self.vdis[vdi.parent].read_only = True
                if geneology.has_key(vdi.parent):
                    geneology[vdi.parent].append(uuid)
                else:
                    geneology[vdi.parent] = [uuid]
            self.virtual_allocation += (vdi.size + \
                                 VDI.VDIMetadataSize(SR.DEFAULT_TAP, vdi.size))

    def _cleanup_leafnodes(self):
        # Loop through VDIs and find unused leafs:
        #   - nodes which have no children
        #   - nodes marked hidden
        for uuid, vdi in self.vdis.iteritems():
            if not geneology.has_key(uuid) and vdi.hidden:
                self._delete_parents(uuid)

    def _delete_parents(self, child):
        hidden = 0
        has_sibling = 0
        parent = ''
        if self.vdis[child].parent:
            parent = self.vdis[child].parent
            hidden = self.vdis[parent].hidden
            for peers in geneology[parent]:
                if peers != child:
                    has_sibling = 1
        child_path = os.path.join(self.path, "%s.%s" % \
                    (child,self.vdis[child].vdi_type))
        try:
            util.ioretry(lambda: os.unlink(child_path))
        except util.CommandException, inst:
            if inst.code != errno.ENOENT:
                raise xs_errors.XenError('VDIRemove', \
                    opterr='failed to remove VDI %s error %d' % \
                    (child_path, inst.code))
        
        size = self.vdis[child].size
        self.virtual_allocation -= (size + \
                                    VDI.VDIMetadataSize(SR.DEFAULT_TAP, size))
        self.vdis[child].deleted = True

        if parent:
            geneology[parent].remove(child)
            if len(geneology[parent]) == 0:
                del geneology[parent]

        if hidden and parent and has_sibling == 0:
            self._delete_parents(parent)

    def _getsize(self):
        st = util.ioretry_stat(lambda: os.statvfs(self.path))
        return st[statvfs.F_BLOCKS] * st[statvfs.F_FRSIZE]
    
    def _getutilisation(self):
        st = util.ioretry_stat(lambda: os.statvfs(self.path))
        return (st[statvfs.F_BLOCKS] - st[statvfs.F_BAVAIL]) * \
                st[statvfs.F_FRSIZE]

    def _replay(self, logentry):
        # all replay commands have the same 5,6,7th arguments
        # vdi_command, sr-uuid, vdi-uuid
        back_cmd = logentry[5].replace("vdi_","")
        target = self.vdi(logentry[7])
        cmd = getattr(target, back_cmd)
        args = []
        for item in logentry[6:]:
            item = item.replace("\n","")
            args.append(item)
        ret = cmd(*args)
        if ret:
            print ret

    def _compare_args(self, a, b):
        try:
            if a[2] != "log:":
                return 1
            if b[2] != "end:" and b[2] != "error:":
                return 1
            if a[3] != b[3]:
                return 1
            if a[4] != b[4]:
                return 1
            return 0
        except:
            return 1

    def _process_replay(self, data):
        logentries=[]
        for logentry in data:
            logentry = logentry.split(" ")
            logentries.append(logentry)
        # we are looking for a log entry that has a log but no end or error
        # wkcfix -- recreate (adjusted) logfile 
        index = 0
        while index < len(logentries)-1:
            if self._compare_args(logentries[index],logentries[index+1]):
                self._replay(logentries[index])
            else:
                # skip the paired one
                index += 1
            # next
            index += 1
 
 
class FileVDI(VDI.VDI):
    def load(self, vdi_uuid):
        self.vdi_type = SR.DEFAULT_TAP
        self.path = os.path.join(self.sr.path, "%s.%s" % \
                                (vdi_uuid,self.vdi_type))
        if util.ioretry(lambda: util.pathexists(self.path)):
            try:
                st = util.ioretry(lambda: os.stat(self.path))
                self.utilisation = long(st.st_size)
            except util.CommandException, inst:
                if inst.code == errno.EIO:
                    raise xs_errors.XenError('VDILoad', \
                          opterr='Failed load VDI information %s' % self.path)
                else:
                    raise xs_errors.XenError('VDIType', \
                          opterr='Invalid VDI type %s' % self.vdi_type)

            try:
                diskinfo = util.ioretry(lambda: self._query_info(self.path))
                if diskinfo.has_key('parent'):
                    self.parent = diskinfo['parent']
                else:
                    self.parent = ''
                self.size = long(diskinfo['size']) * 1024 * 1024
                self.hidden = long(diskinfo['hidden'])
            except util.CommandException, inst:
                raise xs_errors.XenError('VDILoad', \
                      opterr='Failed load VDI information %s' % self.path)

    def create(self, sr_uuid, vdi_uuid, size):
        if util.ioretry(lambda: util.pathexists(self.path)):
            raise xs_errors.XenError('VDIExists')

        # Test the amount of actual disk space
        if ENFORCE_VIRT_ALLOC:
            self.sr._loadvdis()
            reserved = self.sr.virtual_allocation
            sr_size = self.sr._getsize()
            if (sr_size - reserved) < \
               (long(size) + VDI.VDIMetadataSize(SR.DEFAULT_TAP, long(size))):
                raise xs_errors.XenError('SRNoSpace')

        try:
            size_mb = long(size) / (1024 * 1024)
            metasize = VDI.VDIMetadataSize(SR.DEFAULT_TAP, long(size))
            assert(size_mb > 0)
            assert((size_mb + (metasize/(1024*1024))) < MAX_DISK_MB)
            util.ioretry(lambda: self._create(str(size_mb), self.path))
            self.size = util.ioretry(lambda: self._query_v(self.path))
        except util.CommandException, inst:
            raise xs_errors.XenError('VDICreate', opterr='error %d' % inst.code)
        except AssertionError:
            raise xs_errors.XenError('VDISize')
        self.sr.added_vdi(self)

        st = util.ioretry(lambda: os.stat(self.path))
        self.utilisation = long(st.st_size)

        return super(FileVDI, self).get_params(sr_uuid, vdi_uuid)

    def delete(self, sr_uuid, vdi_uuid):
        if not util.ioretry(lambda: util.pathexists(self.path)):
            return

        if self.attached:
            raise xs_errors.XenError('VDIInUse')

        try:
            util.ioretry(lambda: self._mark_hidden(self.path))
        except util.CommandException, inst:
            raise xs_errors.XenError('VDIDelete', opterr='error %d' % inst.code)
        
    def attach(self, sr_uuid, vdi_uuid):
        if not self._checkpath(self.path):
            raise xs_errors.XenError('VDIUnavailable', \
                  opterr='VDI %s unavailable %s' % (vdi_uuid, self.path))
        try:
            self.attached = True
            return super(FileVDI, self).attach(sr_uuid, vdi_uuid)
        except util.CommandException, inst:
            raise xs_errors.XenError('VDILoad', opterr='error %d' % inst.code)

    def detach(self, sr_uuid, vdi_uuid):
        self.attached = False

    def clone(self, sr_uuid, vdi_uuid, dest):
        args = []
        args.append("vdi_clone")
        args.append(sr_uuid)
        args.append(vdi_uuid)
        args.append(dest)


        # Test the amount of actual disk space
        if ENFORCE_VIRT_ALLOC:
            self.sr._loadvdis()
            reserved = self.sr.virtual_allocation
            sr_size = self.sr._getsize()
            if (sr_size - reserved) < \
               ((self.size + VDI.VDIMetadataSize(SR.DEFAULT_TAP, self.size))*2):
                raise xs_errors.XenError('SRNoSpace')

        newuuid = util.gen_uuid()
        src = self.path
        dst = os.path.join(self.sr.path, "%s.%s" % (dest,self.vdi_type))
        newsrc = os.path.join(self.sr.path, "%s.%s" % (newuuid,self.vdi_type))

        if not self._checkpath(src):
            raise xs_errors.XenError('VDIUnavailable', \
                  opterr='VDI %s unavailable %s' % (vdi_uuid, src))

        # wkcfix: multiphase
        util.start_log_entry(self.sr.path, self.path, args)

        # We assume the filehandle has been released
        try:
            try:
                util.ioretry(lambda: os.rename(src,newsrc))
            except util.CommandException, inst:
                if inst.code != errno.ENOENT:
                    self._clonecleanup(src,dst,newsrc)
                    util.end_log_entry(self.sr.path, self.path, ["error"])
                    raise

            try:
                util.ioretry(lambda: self._dualsnap(src, dst, newsrc))
                # mark the original file (in this case, its newsrc) 
                # as hidden so that it does not show up in subsequent scans
                util.ioretry(lambda: self._mark_hidden(newsrc))
            except util.CommandException, inst:
                if inst.code != errno.EIO:
                    self._clonecleanup(src,dst,newsrc)
                    util.end_log_entry(self.sr.path, self.path, ["error"])
                    raise

            #Verify parent locator field of both children and delete newsrc if unused
            try:
                srcparent = util.ioretry(lambda: self._query_p_uuid(src))
                dstparent = util.ioretry(lambda: self._query_p_uuid(dst))
                if srcparent != newuuid and dstparent != newuuid:
                    util.ioretry(lambda: os.unlink(newsrc))
            except:
                pass

        except util.CommandException, inst:
            self._clonecleanup(src,dst,newsrc)
            util.end_log_entry(self.sr.path, self.path, ["error"])
            raise xs_errors.XenError('VDIClone',
                  opterr='VDI clone failed error %d' % inst.code)
        util.end_log_entry(self.sr.path, self.path, ["done"])
            
    def snapshot(self, sr_uuid, vdi_uuid, dest):
        args = []
        args.append("vdi_snapshot")
        args.append(sr_uuid)
        args.append(vdi_uuid)
        args.append(dest)

        # Test the amount of actual disk space
        if ENFORCE_VIRT_ALLOC:
            self.sr._loadvdis()
            reserved = self.sr.virtual_allocation
            sr_size = self.sr._getsize()
            if (sr_size - reserved) < \
                  (self.size + VDI.VDIMetadataSize(SR.DEFAULT_TAP, self.size)):
                raise xs_errors.XenError('SRNoSpace')

        src = self.path
        dst = os.path.join(self.sr.path, "%s.%s" % (dest,self.vdi_type))
        if not self._checkpath(src):
            raise xs_errors.XenError('VDIUnavailable', \
                  opterr='VDI %s unavailable %s' % (vdi_uuid, src))

        # wkcfix: multiphase
        util.start_log_entry(self.sr.path, self.path, args)

        # Vdi_uuid becomes the immutable parent with UUID dest
        # We assume the filehandle has been released
        try:
            try:
                util.ioretry(lambda: os.rename(src,dst))
            except util.CommandException, inst:
                if inst.code != errno.ENOENT:
                    self._snapcleanup(src,dst)
                    util.end_log_entry(self.sr.path, self.path, ["error"])
                    raise

            util.ioretry(lambda: self._singlesnap(src, dst))

        except util.CommandException, inst:
            self._snapcleanup(src,dst)
            util.end_log_entry(self.sr.path, self.path, ["error"])
            raise xs_errors.XenError('VDISnapshot',
                  opterr='VDI snapshot failed error %d' % inst.code)
        util.end_log_entry(self.sr.path, self.path, ["done"])
        
    def lock(self, sr_uuid, vdi_uuid, force, l_uuid):
        return super(FileVDI, self).lock(sr_uuid, vdi_uuid, force, l_uuid)

    def unlock(self, sr_uuid, vdi_uuid, l_uuid):
        return super(FileVDI, self).unlock(sr_uuid, vdi_uuid, l_uuid)

    def get_params(self, sr_uuid, vdi_uuid):
        if not self._checkpath(self.path):
            raise xs_errors.XenError('VDIUnavailable', \
                  opterr='VDI %s unavailable %s' % (vdi_uuid, self.path))
        return super(FileVDI, self).get_params(sr_uuid, vdi_uuid)

    def _dualsnap(self, src, dst, newsrc):
        cmd = [SR.TAPDISK_UTIL, "snapshot", SR.DEFAULT_TAP, src, newsrc]
        text = util.pread(cmd)
        cmd = [SR.TAPDISK_UTIL, "snapshot", SR.DEFAULT_TAP, dst, newsrc]
        text = util.pread(cmd)

    def _singlesnap(self, src, dst):
        cmd = [SR.TAPDISK_UTIL, "snapshot", SR.DEFAULT_TAP, src, dst]
        text = util.pread(cmd)

    def _clonecleanup(self,src,dst,newsrc):
        try:
            util.ioretry(lambda: os.unlink(src))
        except util.CommandException, inst:
            pass
        try:
            util.ioretry(lambda: os.unlink(dst))
        except util.CommandException, inst:
            pass
        try:
            util.ioretry(lambda: os.rename(newsrc,src))
        except util.CommandException, inst:
            pass
      
    def _snapcleanup(self,src,dst):
        try:
            util.ioretry(lambda: os.unlink(dst))
        except util.CommandException, inst:
            pass
        try:
            util.ioretry(lambda: os.rename(src,dst))
        except util.CommandException, inst:
            pass

    def _checkpath(self, path):
        try:
            if not util.ioretry(lambda: util.pathexists(path)):
                return False
            return True
        except util.CommandException, inst:
            raise xs_errors.XenError('EIO', \
                  opterr='IO error checking path %s' % path)

    def _query_v(self, path):
        cmd = [SR.TAPDISK_UTIL, "query", SR.DEFAULT_TAP, "-v", path]
        return long(util.pread(cmd)) * 1024 * 1024

    def _query_p_uuid(self, path):
        cmd = [SR.TAPDISK_UTIL, "query", SR.DEFAULT_TAP, "-p", path]
        parent = util.pread(cmd)
        parent = parent[:-1]
        ls = parent.split('/')
        return ls[len(ls) - 1].replace(SR.DEFAULT_TAP,'')[:-1]

    def _query_info(self, path):
        diskinfo = {}
        cmd = [SR.TAPDISK_UTIL, "query", SR.DEFAULT_TAP, "-vpf", path]
        txt = util.pread(cmd).split('\n')
        diskinfo['size'] = txt[0]
        if txt[1].find("has no parent") == -1:
            diskinfo['parent'] = txt[1].split('/')[-1].replace(".%s" % SR.DEFAULT_TAP,"")
        diskinfo['hidden'] = txt[2].split()[1]
        return diskinfo

    def _create(self, size, path):
        cmd = [SR.TAPDISK_UTIL, "create", SR.DEFAULT_TAP, size, path]
        text = util.pread(cmd)

    def _mark_hidden(self, path):
        cmd = [SR.TAPDISK_UTIL, "set", SR.DEFAULT_TAP, path, "hidden", "1"]
        text = util.pread(cmd)
        self.hidden = 1

if __name__ == '__main__':
    SRCommand.run(FileSR, DRIVER_INFO)
else:
    SR.registerSR(FileSR)
