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
# ISOSR: remote iso storage repository

import SR, VDI, SRCommand, util
import nfs
import os, re
import xs_errors
import xmlrpclib
import string

CAPABILITIES = ["VDI_CREATE", "VDI_DELETE", "VDI_ATTACH", "VDI_DETACH", 
                "SR_SCAN", "SR_ATTACH", "SR_DETACH"]

CONFIGURATION = \
    [ [ 'location', 'path to mount (required) (e.g. server:/path)' ], 
      [ 'options', 
        'extra options to pass to mount (deprecated) (e.g. \'-o ro\')' ],
      [ 'type','cifs or nfs'],
      nfs.NFS_VERSION]

DRIVER_INFO = {
    'name': 'ISO',
    'description': 'Handles CD images stored as files in iso format',
    'vendor': 'Citrix Systems Inc',
    'copyright': '(C) 2008 Citrix Systems Inc',
    'driver_version': '1.0',
    'required_api_version': '1.0',
    'capabilities': CAPABILITIES,
    'configuration': CONFIGURATION
    }

TYPE = "iso"

def is_image_utf8_compatible(s):
    regex = re.compile("\.iso$|\.img$", re.I)
    if regex.search(s) == None:
        return False

    # Check for extended characters
    if type(s) == str:
        try:
            s.decode('utf-8')
        except UnicodeDecodeError, e:
            util.SMlog("WARNING: This string is not UTF-8 compatible.")
            return False
    return True 

class ISOSR(SR.SR):
    """Local file storage repository"""

# Some helper functions:
    def _checkmount(self):
        """Checks that the mountpoint exists and is mounted"""
        if not util.pathexists(self.mountpoint):
            return False
        try:
            ismount = util.ismount(self.mountpoint)
        except util.CommandException, inst:
            return False
        return ismount

    def _checkTargetStr(self, location):
        if not self.dconf.has_key('type'):
            return
        if self.dconf['type'] == 'cifs':
            tgt = ''
            if re.search('^//',location):
                tgt = location.split('/')[2]
            elif re.search(r'^\\',location):
                l = location.split('\\')
                for i in location.split('\\'):
                    if i:
                        tgt = i
                        break
            if not tgt:
                raise xs_errors.XenError('ISOLocationStringError')
        else:
            if location.find(':') == -1:
                raise xs_errors.XenError('ISOLocationStringError')
            tgt = location.split(':')[0]

        try:
            util._convertDNS(tgt)
        except:
            raise xs_errors.XenError('DNSError')

    uuid_file_regex = re.compile(
        "([0-9a-f]{8}-(([0-9a-f]{4})-){3}[0-9a-f]{12})\.(iso|img)", re.I)
    def _loadvdis(self):
        """Scan the directory and get uuids either from the VDI filename, \
        or by creating a new one."""
        if self.vdis:
            return

        for name in filter(is_image_utf8_compatible,
                util.listdir(self.path, quiet = True)):
	    fileName = self.path + "/" + name
            if os.path.isdir(fileName):
                util.SMlog("_loadvdis : %s is a directory. Ignore" % fileName)
                continue

            # CA-80254: Check for iso/img files whose name consists of extended
            # characters.
            try:
                name.decode('ascii')
            except UnicodeDecodeError:
                raise xs_errors.XenError('CIFSExtendedCharsNotSupported', \
                        opterr = 'The repository contains at least one file whose name consists of extended characters.')

            self.vdis[name] = ISOVDI(self, name)
            # Set the VDI UUID if the filename is of the correct form.
            # Otherwise, one will be generated later in VDI._db_introduce.
            m = self.uuid_file_regex.match(name)
            if m:
                self.vdis[name].uuid = m.group(1)

        # Synchronise the read-only status with existing VDI records
        __xenapi_records = util.list_VDI_records_in_sr(self)
        __xenapi_locations = {}
        for vdi in __xenapi_records.keys():
            __xenapi_locations[__xenapi_records[vdi]['location']] = vdi
        for vdi in self.vdis.values():
            if vdi.location in __xenapi_locations:
                v = __xenapi_records[__xenapi_locations[vdi.location]]
                sm_config = v['sm_config']
                if sm_config.has_key('created'):
                    vdi.sm_config['created'] = sm_config['created']
                    vdi.read_only = False

# Now for the main functions:    
    def handles(type):
        """Do we handle this type?"""
        if type == TYPE:
            return True
        return False
    handles = staticmethod(handles)

    def content_type(self, sr_uuid):
        """Returns the content_type XML""" 
        return super(ISOSR, self).content_type(sr_uuid)

    vdi_path_regex = re.compile("[a-z0-9.-]+\.(iso|img)", re.I)
    def vdi(self, uuid):
        """Create a VDI class.  If the VDI does not exist, we determine
        here what its filename should be."""

        filename = util.to_plain_string(self.srcmd.params.get('vdi_location'))
        if filename is None:
            smconfig = self.srcmd.params.get('vdi_sm_config')
            if smconfig is None:
                # uh, oh, a VDI.from_uuid()
                import XenAPI
                _VDI = self.session.xenapi.VDI
                try:
                    vdi_ref  = _VDI.get_by_uuid(uuid)
                except XenAPI.Failure, e:
                    if e.details[0] != 'UUID_INVALID': raise
                else:
                    filename = _VDI.get_location(vdi_ref)

        if filename is None:
            # Get the filename from sm-config['path'], or use the UUID
            # if the path param doesn't exist.
            if smconfig and smconfig.has_key('path'):
                filename = smconfig['path']
                if not self.vdi_path_regex.match(filename):
                    raise xs_errors.XenError('VDICreate', \
                                                 opterr='Invalid path "%s"' % filename)
            else:
                filename = '%s.img' % uuid

        return ISOVDI(self, filename)

    def load(self, sr_uuid):
        """Initialises the SR"""
        # First of all, check we've got the correct keys in dconf
        if not self.dconf.has_key('location'):
            raise xs_errors.XenError('ConfigLocationMissing')

        # Construct the path we're going to mount under:
        if self.dconf.has_key("legacy_mode"):
            self.mountpoint = util.to_plain_string(self.dconf['location'])
        else:
            # Verify the target address
            self._checkTargetStr(self.dconf['location'])
            self.mountpoint = os.path.join(SR.MOUNT_BASE, sr_uuid)
            
        # Add on the iso_path value if there is one
        if self.dconf.has_key("iso_path"):
            iso_path = util.to_plain_string(self.dconf['iso_path'])
            if iso_path.startswith("/"):
                iso_path=iso_path[1:]
            self.path = os.path.join(self.mountpoint, iso_path)
        else:
            self.path = self.mountpoint

        # Handle optional dconf attributes
        self.nfsversion = nfs.validate_nfsversion(self.dconf.get('nfsversion'))

        # Some info we need:
        self.sr_vditype = 'phy'
        self.credentials = None

    def delete(self, sr_uuid):
        pass
 
    def attach(self, sr_uuid):
        """Std. attach"""
        # Very-Legacy mode means the ISOs are in the local fs - so no need to attach.
        if self.dconf.has_key('legacy_mode'):
            # Verify path exists
            if not os.path.exists(self.mountpoint):
                raise xs_errors.XenError('ISOLocalPath')
            return
        
        # Check whether we're already mounted
        if self._checkmount():
            return

        # Create the mountpoint if it's not already there
        if not util.isdir(self.mountpoint):
            util.makedirs(self.mountpoint)

        mountcmd=[]
        location = util.to_plain_string(self.dconf['location'])

        self.credentials = os.path.join("/tmp", util.gen_uuid())
        if self.dconf.has_key('type'):
            if self.dconf['type']=='cifs':
                # CIFS specific stuff
                # Check for username and password
                mountcmd=["mount.cifs", location, self.mountpoint]
                self.appendCIFSPasswordOptions(mountcmd)
        else:
            # Not-so-legacy mode:
            if self.dconf.has_key('options'):
                options = self.dconf['options'].split(' ')
                options = filter(lambda x: x != "", options)

                mountcmd=["mount", location, self.mountpoint]
                mountcmd.extend(options)
            else:
                mountcmd=["mount", location, self.mountpoint]
            self.appendCIFSPasswordOptions(mountcmd)
        # Mount!
        try:
            # For NFS, do a soft mount with tcp as protocol. Since ISO SR is
            # going to be r-only, a failure in nfs link can be reported back
            # to the process waiting.
            if self.dconf.has_key('type') and self.dconf['type']!='cifs':
                serv_path = location.split(':')
                nfs.soft_mount(self.mountpoint, serv_path[0], serv_path[1],
                               'tcp', nfsversion=self.nfsversion)
            else:
                util.pread(mountcmd, True)
        except util.CommandException, inst:
            self._cleanupcredentials()
            raise xs_errors.XenError('ISOMountFailure')
        self._cleanupcredentials()

        # Check the iso_path is accessible
        if not self._checkmount():
            self.detach(sr_uuid)
            raise xs_errors.XenError('ISOSharenameFailure')                        

    def appendCIFSPasswordOptions(self, mountcmd):
        if self.dconf.has_key('username') \
                and (self.dconf.has_key('cifspassword') or self.dconf.has_key('cifspassword_secret')):
            username = self.dconf['username'].replace("\\","/")
            if self.dconf.has_key('cifspassword_secret'):
                password = util.get_secret(self.session, self.dconf['cifspassword_secret'])
            else:
                password = self.dconf['cifspassword']

            username = util.to_plain_string(username)
            password = util.to_plain_string(password)

            # Open credentials file and truncate
            f = open(self.credentials, 'w')
            f.write("username=%s\npassword=%s\n" % (username,password))
            f.close()            
            credentials = "credentials=%s" % self.credentials            
            mountcmd.extend(["-o", credentials])

    def _cleanupcredentials(self):
        if self.credentials and os.path.exists(self.credentials):
            os.unlink(self.credentials)

    def detach(self, sr_uuid):
        """Std. detach"""
        # This handles legacy mode too, so no need to check
        if not self._checkmount():
            return 
 
        try:
            util.pread(["umount", self.mountpoint]);
        except util.CommandException, inst:
            raise xs_errors.XenError('NFSUnMount', \
                                         opterr = 'error is %d' % inst.code)

    def scan(self, sr_uuid):
        """Scan: see _loadvdis"""
        if not util.isdir(self.path):
            raise xs_errors.XenError('SRUnavailable', \
                    opterr = 'no such directory %s' % self.path)            

        if (not self.dconf.has_key('legacy_mode')) and (not self._checkmount()):
            raise xs_errors.XenError('SRUnavailable', \
                    opterr = 'directory not mounted: %s' % self.path) 

        #try:
        if not self.vdis:
            self._loadvdis()
        self.physical_size = util.get_fs_size(self.path)
        self.physical_utilisation = util.get_fs_utilisation(self.path)
        self.virtual_allocation = self.physical_size

        if self.dconf.has_key("legacy_mode"):
            # Out of all the xs-tools ISOs which exist in this dom0, we mark
            # only one as the official one.

            # Pass 1: find the latest version
            latest_build_vdi = None
            latest_build_number = "0"
            for vdi_name in self.vdis:
                vdi = self.vdis[vdi_name]

                if latest_build_vdi == None:
                    latest_build_vdi = vdi.location
                    latest_build_number = "0"

                if vdi.sm_config.has_key('xs-tools-build'):
                    bld = vdi.sm_config['xs-tools-build']
                    if bld >= latest_build_number:
                        latest_build_vdi = vdi.location
                        latest_build_number = bld

            # Pass 2: mark all VDIs accordingly
            for vdi_name in self.vdis:
                vdi = self.vdis[vdi_name]
                if vdi.location == latest_build_vdi:
                    vdi.sm_config['xs-tools'] = "true"
                else:
                    if vdi.sm_config.has_key("xs-tools"):
                        del vdi.sm_config['xs-tools']


            # Synchronise the VDIs: this will update the sm_config maps of current records
            scanrecord = SR.ScanRecord(self)
            scanrecord.synchronise_new()
            scanrecord.synchronise_existing()

            # Everything that looks like an xs-tools ISO but which isn't the
            # primary one will also be renamed "Old version of ..."
            sr = self.session.xenapi.SR.get_by_uuid(sr_uuid)
            all_vdis = self.session.xenapi.VDI.get_all_records_where("field \"SR\" = \"%s\"" % sr)
            for vdi_ref in all_vdis.keys():
                vdi = all_vdis[vdi_ref]
                if vdi['sm_config'].has_key('xs-tools-version'):
                    if vdi['sm_config'].has_key('xs-tools'):
                        self.session.xenapi.VDI.set_name_label(vdi_ref, "xs-tools.iso")
                    else:
                        self.session.xenapi.VDI.set_name_label(vdi_ref, "Old version of xs-tools.iso")


            # never forget old VDI records to cope with rolling upgrade
            for location in scanrecord.gone:
                vdi = scanrecord.get_xenapi_vdi(location)
                util.SMlog("Marking previous version of tools ISO: location=%s uuid=%s" % (vdi['location'], vdi['uuid']))
                vdi = self.session.xenapi.VDI.get_by_uuid(vdi['uuid'])
                name_label = self.session.xenapi.VDI.get_name_label(vdi)
                if not(name_label.startswith("Old version of ")):
                    self.session.xenapi.VDI.set_name_label(vdi, "Old version of " + name_label)
                # Mark it as missing for informational purposes only
                self.session.xenapi.VDI.set_missing(vdi, True)
                self.session.xenapi.VDI.remove_from_sm_config(vdi, 'xs-tools' )

        else:
            return super(ISOSR, self).scan(sr_uuid)

    def create(self, sr_uuid, size):
        self.attach(sr_uuid)
        if self.dconf.has_key('type'):
            smconfig = self.session.xenapi.SR.get_sm_config(self.sr_ref)
            smconfig['iso_type'] = self.dconf['type']
            self.session.xenapi.SR.set_sm_config(self.sr_ref, smconfig)

        # CA-80254: Check for iso/img files whose name consists of extended
        # characters.
        for f in util.listdir(self.path, quiet = True):
            if is_image_utf8_compatible(f):
                try:
                    f.decode('ascii')
                except UnicodeDecodeError:
                    raise xs_errors.XenError('CIFSExtendedCharsNotSupported',
                            opterr = 'The repository contains at least one file whose name consists of extended characters.')

        self.detach(sr_uuid)

        
class ISOVDI(VDI.VDI):
    def load(self, vdi_uuid):
        # Nb, in the vdi_create call, the filename is unset, so the following
        # will fail.
        self.vdi_type = "iso"
        try:
            stat = os.stat(self.path)
            self.utilisation = long(stat.st_size)
            self.size = long(stat.st_size)
            self.label = self.filename
        except:
            pass

    def __init__(self, mysr, filename):
        self.path = os.path.join(mysr.path, filename)
        VDI.VDI.__init__(self, mysr, None)
        self.location = filename
        self.filename = filename
        self.read_only = True                
        self.label = filename
        self.sm_config = {}
        if mysr.dconf.has_key("legacy_mode"):
            if filename.startswith("xs-tools"):
                self.label = "xs-tools.iso"
                # Mark this as a Tools CD
                # self.sm_config['xs-tools'] = 'true'
                # Extract a version string, if present
                vsn = filename[len("xs-tools"):][:-len(".iso")].strip("-").split("-",1)
                # "4.1.0"
                if len(vsn) == 1:
                    build_number="0" # string
                    product_version=vsn[0]
                # "4.1.0-1234"
                elif len(vsn) > 1:
                    build_number=vsn[1]
                    product_version=vsn[0]
                else:
                    build_number=0
                    product_version="unknown"
                util.SMlog("version=%s build=%s" % (product_version, build_number))
                self.sm_config['xs-tools-version'] = product_version
                self.sm_config['xs-tools-build'] = build_number

    def detach(self, sr_uuid, vdi_uuid):
        pass

    def attach(self, sr_uuid, vdi_uuid):
        try:
            os.stat(self.path)        
            return super(ISOVDI, self).attach(sr_uuid, vdi_uuid)
        except:
            raise xs_errors.XenError('VDIMissing')

    def create(self, sr_uuid, vdi_uuid, size):
        self.uuid = vdi_uuid
        self.path = os.path.join(self.sr.path, self.filename)
        self.size = size
        self.utilisation = 0L
        self.read_only = False
        self.sm_config = self.sr.srcmd.params['vdi_sm_config']
        self.sm_config['created'] = util._getDateString()

        if util.pathexists(self.path):
            raise xs_errors.XenError('VDIExists')

        try:
            handle = open(self.path,"w")
            handle.truncate(size)
            handle.close()
            self._db_introduce()
            return super(ISOVDI, self).get_params()
        except Exception, exn:
            util.SMlog("Exception when creating VDI: %s" % exn)
            raise xs_errors.XenError('VDICreate', \
                     opterr='could not create file: "%s"' % self.path)

    def delete(self, sr_uuid, vdi_uuid):
        util.SMlog("Deleting...")

        self.uuid = vdi_uuid
        self._db_forget()

        if not util.pathexists(self.path):
            return

        try:
            util.SMlog("Unlinking...")
            os.unlink(self.path)
            util.SMlog("Done...")
        except:
            raise xs_errors.XenError('VDIDelete')

    # delete, update, introduce unimplemented. super class will raise
    # exceptions 

if __name__ == '__main__':
    SRCommand.run(ISOSR, DRIVER_INFO)
else:
    SR.registerSR(ISOSR)
