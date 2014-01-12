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
# SR: Base class for storage repositories
#

import VDI
import xml.dom.minidom
import errno
import xs_errors
import XenAPI, xmlrpclib, util
import copy, os

MOUNT_BASE = '/var/run/sr-mount'
DEFAULT_TAP = 'vhd'
TAPDISK_UTIL = '/usr/sbin/td-util'
MASTER_LVM_CONF = '/etc/lvm/master'

# LUN per VDI key for XenCenter
LUNPERVDI = "LUNperVDI"

class SRException(Exception):
    """Exception raised by storage repository operations"""
    errno = errno.EINVAL
    def __init__(self, reason):
        Exception.__init__(self, reason)

    def toxml(self):
        return xmlrpclib.dumps(xmlrpclib.Fault(int(self.errno), str(self)), "", True)

class SROSError(SRException):
    """Wrapper for OSError"""
    def __init__(self, errno, reason):
        self.errno = errno
        Exception.__init__(self, reason)

backends = []
def registerSR(SRClass):
    """Register SR with handler. All SR subclasses should call this in 
       the module file
    """
    backends.append(SRClass)

def driver(type):
    """Find the SR for the given dconf string"""
    for d in backends:
        if d.handles(type):
            return d
    raise xs_errors.XenError('SRUnknownType')

class SR(object):
    """Semi-abstract storage repository object.

    Attributes:
      uuid: string, UUID
      label: string
      description: string
      vdis: dictionary, VDI objects indexed by UUID
      physical_utilisation: int, bytes consumed by VDIs
      virtual_allocation: int, bytes allocated to this repository (virtual)
      physical_size: int, bytes consumed by this repository
      sr_vditype: string, repository type
    """
    def handles(type):
        """Returns True if this SR class understands the given dconf string"""
        return False
    handles = staticmethod(handles)

    def __init__(self, srcmd, sr_uuid):
        """Base class initializer. All subclasses should call SR.__init__ 
           in their own
        initializers.

        Arguments:
          srcmd: SRCommand instance, contains parsed arguments
        """
        try:
            self.srcmd = srcmd
            self.dconf = srcmd.dconf
            if srcmd.params.has_key('session_ref'):
                self.session_ref = srcmd.params['session_ref']
                self.session = XenAPI.xapi_local()
                self.session._session = self.session_ref
                if 'subtask_of' in self.srcmd.params:
                    self.session.transport.add_extra_header('Subtask-of', self.srcmd.params['subtask_of'])
            else:
                self.session = None

            if 'host_ref' not in self.srcmd.params:
                self.host_ref = ""
            else:
                self.host_ref = self.srcmd.params['host_ref']

            self.sr_ref = self.srcmd.params.get('sr_ref')

	    if 'device_config' in self.srcmd.params:
                if self.dconf.get("SRmaster") == "true":
                    os.environ['LVM_SYSTEM_DIR'] = MASTER_LVM_CONF

        except Exception, e:
            raise e
            raise xs_errors.XenError('SRBadXML')

        self.uuid = sr_uuid

        self.label = ''
        self.description = ''
        self.cmd = srcmd.params['command']
        self.vdis = {}
        self.physical_utilisation = 0
        self.virtual_allocation = 0
        self.physical_size = 0
        self.sr_vditype = ''
        self.passthrough = False
        # XXX: if this is really needed then we must make a deep copy
        self.original_srcmd = copy.deepcopy(self.srcmd)
        self.default_vdi_visibility = True
        self.sched = 'noop'
        self._mpathinit()
        self.direct = False
        self.ops_exclusive = []
        self.driver_config = {}

        self.load(sr_uuid)

    @staticmethod
    def from_uuid(session, sr_uuid):
        import imp

        _SR = session.xenapi.SR
        sr_ref = _SR.get_by_uuid(sr_uuid)
        sm_type = _SR.get_type(sr_ref)

        # NB. load the SM driver module

        _SM = session.xenapi.SM
        sms = _SM.get_all_records_where('field "type" = "%s"' % sm_type)
        sm_ref, sm = sms.popitem()
        assert not sms

        driver_path = _SM.get_driver_filename(sm_ref)
        driver_real = os.path.realpath(driver_path)
        module_name = os.path.basename(driver_path)

        module = imp.load_source(module_name, driver_real)
        target = driver(sm_type)

        # NB. get the host pbd's device_config

        host_ref = util.get_localhost_uuid(session)

        _PBD = session.xenapi.PBD
        pbds = _PBD.get_all_records_where('field "SR" = "%s" and' % sr_ref +
                                          'field "host" = "%s"' % host_ref)
        pbd_ref, pbd = pbds.popitem()
        assert not pbds

        device_config = _PBD.get_device_config(pbd_ref)

        # NB. make srcmd, to please our supersized SR constructor.
        # FIXME

        from SRCommand import SRCommand
        cmd = SRCommand(module.DRIVER_INFO)
        cmd.dconf  = device_config
        cmd.params = { 'session_ref'    : session._session,
                       'host_ref'       : host_ref,
                       'device_config'  : device_config,
                       'sr_ref'         : sr_ref,
                       'sr_uuid'        : sr_uuid,
                       'command'        : 'nop' }

        return target(cmd, sr_uuid)

    def block_setscheduler(self, dev):
        try:
            realdev = os.path.realpath(dev)
            disk    = util.diskFromPartition(realdev)

            # the normal case: the sr default scheduler (typically noop),
            # potentially overridden by SR.other_config:scheduler
            other_config = self.session.xenapi.SR.get_other_config(self.sr_ref)
            sched = other_config.get('scheduler')
            if not sched:
                sched = self.sched

            # special case: CFQ if the underlying disk holds dom0's file systems.
            if disk in util.dom0_disks():
                sched = 'cfq'

            util.SMlog("Block scheduler: %s (%s) wants %s" % (dev, disk, sched))
            util.set_scheduler(realdev[5:], sched)

        except Exception, e:
            util.SMlog("Failed to set block scheduler on %s: %s" % (dev, e))

    def _addLUNperVDIkey(self):
        try:
            self.session.xenapi.SR.add_to_sm_config(self.sr_ref, LUNPERVDI, "true")
        except:
            pass        

    def create(self, uuid, size):
        """Create this repository.
        This operation may delete existing data.

        The operation is NOT idempotent. The operation will fail
        if an SR of the same UUID and driver type already exits.

        Returns:
          None
        Raises:
          SRUnimplementedMethod
        """
        raise xs_errors.XenError('Unimplemented')

    def delete(self, uuid):
        """Delete this repository and its contents.

        This operation IS idempotent -- it will succeed if the repository
        exists and can be deleted or if the repository does not exist.
        The caller must ensure that all VDIs are deactivated and detached
        and that the SR itself has been detached before delete(). 
        The call will FAIL if any VDIs in the SR are in use.

        Returns:
          None
        Raises:
          SRUnimplementedMethod
        """
        raise xs_errors.XenError('Unimplemented')

    def update(self, uuid):
        """Refresh the fields in the SR object

        Returns:
          None
        Raises:
          SRUnimplementedMethod
        """
        # no-op unless individual backends implement it
        return
    
    def attach(self, uuid):
        """Initiate local access to the SR. Initialises any 
        device state required to access the substrate.

        Idempotent.

        Returns:
          None
        Raises:
          SRUnimplementedMethod
        """
        raise xs_errors.XenError('Unimplemented')

    def detach(self, uuid):
        """Remove local access to the SR. Destroys any device 
        state initiated by the sr_attach() operation.

        Idempotent. All VDIs must be detached in order for the operation
        to succeed.

        Returns:
          None
        Raises:
          SRUnimplementedMethod
        """
        raise xs_errors.XenError('Unimplemented')

    def probe(self):
        """Perform a backend-specific scan, using the current dconf.  If the
        dconf is complete, then this will return a list of the SRs present of
        this type on the device, if any.  If the dconf is partial, then a
        backend-specific scan will be performed, returning results that will
        guide the user in improving the dconf.

        Idempotent.

        xapi will ensure that this is serialised wrt any other probes, or
        attach or detach operations on this host.

        Returns:
          An XML fragment containing the scan results.  These are specific
          to the scan being performed, and the current backend.
        Raises:
          SRUnimplementedMethod
        """
        raise xs_errors.XenError('Unimplemented')

    def scan(self, uuid):
        """
        Returns:
        """
        # Update SR parameters
        self._db_update()
        # Synchronise VDI list
        scanrecord = ScanRecord(self)
        scanrecord.synchronise()

    def replay(self, uuid):
        """Replay a multi-stage log entry

        Returns:
          None
        Raises:
          SRUnimplementedMethod
        """
        raise xs_errors.XenError('Unimplemented')
    
    def content_type(self, uuid):
        """Returns the 'content_type' of an SR as a string"""
        return xmlrpclib.dumps((str(self.sr_vditype),), "", True)
    
    def load(self, sr_uuid):
        """Post-init hook"""
        pass

    def vdi(self, uuid):
        """Return VDI object owned by this repository"""
        if not self.vdis.has_key(uuid):
            self.vdis[uuid] = VDI.VDI(self, uuid)
        raise xs_errors.XenError('Unimplemented')
        return self.vdis[uuid]

    def forget_vdi(self, uuid):
        vdi = self.session.xenapi.VDI.get_by_uuid(uuid)
        self.session.xenapi.VDI.db_forget(vdi)

    def cleanup(self):
        # callback after the op is done
        pass

    def _db_update(self):
        sr = self.session.xenapi.SR.get_by_uuid(self.uuid)
        self.session.xenapi.SR.set_virtual_allocation(sr, str(self.virtual_allocation))
        self.session.xenapi.SR.set_physical_size(sr, str(self.physical_size))
        self.session.xenapi.SR.set_physical_utilisation(sr, str(self.physical_utilisation))

    def _toxml(self):
        dom = xml.dom.minidom.Document()
        element = dom.createElement("sr")
        dom.appendChild(element)

        #Add default uuid, physical_utilisation, physical_size and 
        # virtual_allocation entries
        for attr in ('uuid', 'physical_utilisation', 'virtual_allocation', \
                     'physical_size'):
            try:
                aval = getattr(self, attr)
            except AttributeError:
                raise xs_errors.XenError('InvalidArg', \
                      opterr='Missing required field [%s]' % attr)

            entry = dom.createElement(attr)
            element.appendChild(entry)           
            textnode = dom.createTextNode(str(aval))
            entry.appendChild(textnode)

        #Add the default_vdi_visibility entry
        entry = dom.createElement('default_vdi_visibility')
        element.appendChild(entry)
        if not self.default_vdi_visibility:
            textnode = dom.createTextNode('False')
        else:
            textnode = dom.createTextNode('True')
        entry.appendChild(textnode)
        
        #Add optional label and description entries
        for attr in ('label', 'description'):
            try:
                aval = getattr(self, attr)
            except AttributeError:
                continue
            if aval:
                entry = dom.createElement(attr)
                element.appendChild(entry)           
                textnode = dom.createTextNode(str(aval))
                entry.appendChild(textnode)

        # Create VDI sub-list
        if self.vdis:
            for uuid in self.vdis:
                if not self.vdis[uuid].deleted:
                    vdinode = dom.createElement("vdi")
                    element.appendChild(vdinode)
                    self.vdis[uuid]._toxml(dom, vdinode)

        return dom
    
    def _fromxml(self, str, tag):
        dom = xml.dom.minidom.parseString(str)
        objectlist = dom.getElementsByTagName(tag)[0]
        taglist = {}
        for node in objectlist.childNodes:
            taglist[node.nodeName] = ""
            for n in node.childNodes:
                if n.nodeType == n.TEXT_NODE:
                    taglist[node.nodeName] += n.data
        return taglist

    def _isvalidpathstring(self, path):
        if not path.startswith("/"):
            return False
        l = self._splitstring(path)
        for char in l:
            if char.isalpha():
                continue
            elif char.isdigit():
                continue
            elif char in ['/','-','_','.',':']:
                continue
            else:
                return False
        return True

    def _splitstring(self, str):
        elementlist = []
        for i in range(0, len(str)):
            elementlist.append(str[i])
        return elementlist

    def _mpathinit(self):
        self.mpath = "false"
        try:
            if self.dconf.has_key('multipathing') and \
                   self.dconf.has_key('multipathhandle'):
                self.mpath = self.dconf['multipathing']
                self.mpathhandle = self.dconf['multipathhandle']
            else:
                hconf = self.session.xenapi.host.get_other_config(self.host_ref)
                self.mpath = hconf['multipathing']
                self.mpathhandle = hconf['multipathhandle']

            if self.mpath != "true":
                self.mpath = "false"
                self.mpathhandle = "null"
                
            if not os.path.exists("/opt/xensource/sm/mpath_%s.py" % self.mpathhandle):
                raise
        except:
            self.mpath = "false"
            self.mpathhandle = "null"
        module_name = "mpath_%s" % self.mpathhandle
        self.mpathmodule = __import__(module_name)

    def _mpathHandle(self):
        if self.mpath == "true":
            self.mpathmodule.activate()
        else:
            self.mpathmodule.deactivate()

    def _pathrefresh(self, obj):
        SCSIid = getattr(self, 'SCSIid')
        self.dconf['device'] = self.mpathmodule.path(SCSIid)
        super(obj, self).load(self.uuid)

    def _setMultipathableFlag(self, SCSIid=''):
        try:
            sm_config = self.session.xenapi.SR.get_sm_config(self.sr_ref)
            sm_config['multipathable'] = 'true'
            self.session.xenapi.SR.set_sm_config(self.sr_ref, sm_config)

            if self.mpath == "true" and len(SCSIid):
                cmd = ['/opt/xensource/sm/mpathcount.py',SCSIid]
                util.pread2(cmd)
        except:
            pass

    
class ScanRecord:
    def __init__(self, sr):
        self.sr = sr
        self.__xenapi_locations = {}
        self.__xenapi_records = util.list_VDI_records_in_sr(sr)
        for vdi in self.__xenapi_records.keys():
            self.__xenapi_locations[util.to_plain_string(self.__xenapi_records[vdi]['location'])] = vdi
        self.__sm_records = {}
        for vdi in sr.vdis.values():
            # We initialise the sm_config field with the values from the database
            # The sm_config_overrides contains any new fields we want to add to
            # sm_config, and also any field to delete (by virtue of having 
            # sm_config_overrides[key]=None)
            try:
                if not hasattr(vdi, "sm_config"):
                    vdi.sm_config = self.__xenapi_records[self.__xenapi_locations[vdi.location]]['sm_config'].copy()
            except:
                util.SMlog("missing config for vdi: %s" % vdi.location)
                vdi.sm_config = {}

            vdi._override_sm_config(vdi.sm_config)

            self.__sm_records[vdi.location] = vdi

        xenapi_locations = set(self.__xenapi_locations.keys())
        sm_locations = set(self.__sm_records.keys())

        # These ones are new on disk
        self.new = sm_locations.difference(xenapi_locations)
        # These have disappeared from the disk
        self.gone = xenapi_locations.difference(sm_locations)
        # These are the ones which are still present but might have changed...
        existing = sm_locations.intersection(xenapi_locations)
        # Synchronise the uuid fields using the location as the primary key
        # This ensures we know what the UUIDs are even though they aren't stored
        # in the storage backend.
        for location in existing:
            sm_vdi = self.get_sm_vdi(location)
            xenapi_vdi = self.get_xenapi_vdi(location)
            sm_vdi.uuid = util.default(sm_vdi, "uuid", lambda: xenapi_vdi['uuid']) 
                
        # Only consider those whose configuration looks different
        self.existing = filter(lambda x:not(self.get_sm_vdi(x).in_sync_with_xenapi_record(self.get_xenapi_vdi(x))), existing)

        if len(self.new) <> 0:
            util.SMlog("new VDIs on disk: " + repr(self.new))
        if len(self.gone) <> 0:
            util.SMlog("VDIs missing from disk: " + repr(self.gone))
        if len(self.existing) <> 0:
            util.SMlog("VDIs changed on disk: " + repr(self.existing))

    def get_sm_vdi(self, location):
        return self.__sm_records[location]

    def get_xenapi_vdi(self, location):
        return self.__xenapi_records[self.__xenapi_locations[location]]

    def all_xenapi_locations(self):
	return set(self.__xenapi_locations.keys())

    def synchronise_new(self):
        """Add XenAPI records for new disks"""
        for location in self.new:
            vdi = self.get_sm_vdi(location)
            util.SMlog("Introducing VDI with location=%s" % (vdi.location))
            vdi._db_introduce()
            
    def synchronise_gone(self):
        """Delete XenAPI record for old disks"""
        for location in self.gone:
            vdi = self.get_xenapi_vdi(location)
            util.SMlog("Forgetting VDI with location=%s uuid=%s" % (util.to_plain_string(vdi['location']), vdi['uuid']))
            try:
                self.sr.forget_vdi(vdi['uuid'])
            except XenAPI.Failure, e:
                if util.isInvalidVDI(e):
                   util.SMlog("VDI %s not found, ignoring exception" \
                           % vdi['uuid'])
                else:
                   raise

    def synchronise_existing(self):
        """Update existing XenAPI records"""
        for location in self.existing:
            vdi = self.get_sm_vdi(location)
            
            util.SMlog("Updating VDI with location=%s uuid=%s" % (vdi.location, vdi.uuid))
            vdi._db_update()
            
    def synchronise(self):
        """Perform the default SM -> xenapi synchronisation; ought to be good enough
        for most plugins."""
        self.synchronise_new()
        self.synchronise_gone()
        self.synchronise_existing()


