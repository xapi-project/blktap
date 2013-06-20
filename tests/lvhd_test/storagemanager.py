import os
import sys
import re
import stat
import time
import xmlrpclib
import random
import string

import tutil
sys.path.append("/opt/xensource/sm")
import util
import lvutil
import vhdutil
import lvhdutil
import lvmcache
import cleanup

LOG_LEVEL_CMD = 3
LOG_LEVEL_SUB = 4

CMD_NUM_RETRIES = 3
CMD_RETRY_PERIOD = 2

class SMException(Exception):
    pass

class StorageManager:
    "Python SM API"

    # storage target types
    STORAGE_TYPE_RAW  = 0
    STORAGE_TYPE_LVM  = 1
    STORAGE_TYPE_VHD  = 2
    STORAGE_TYPE_LVHD = 3
    STORAGE_TYPE_NAME = {
            STORAGE_TYPE_RAW    :"raw",
            STORAGE_TYPE_LVM    :"LVM",
            STORAGE_TYPE_VHD    :"VHD",
            STORAGE_TYPE_LVHD   :"LVHD",
    }
    STORAGE_TYPE_SR_NAME = {
            STORAGE_TYPE_LVM    :"lvm",
            STORAGE_TYPE_VHD    :"ext",
            STORAGE_TYPE_LVHD   :"lvhd",
    }

    SR_LABEL = "TestSR"
    VG_PREFIX = "VG_XenStorage-"

    FIRST_VBD_LETTER = 'd'
    LAST_VBD_LETTER = 'p'

    def getInstance(logger):
        return StorageManagerCLI(logger)
    getInstance = staticmethod(getInstance)

    def getOverheadVHDEmpty(virtSize):
        """Calculate the VHD space overhead (metadata size) for an empty VDI of
        size virtual_size"""
        return vhdutil.calcOverheadEmpty(virtSize)
    getOverheadVHDEmpty = staticmethod(getOverheadVHDEmpty)

    def getOverheadVHDFull(virtSize):
        return vhdutil.calcOverheadFull(virtSize)
    getOverheadVHDFull = staticmethod(getOverheadVHDFull)

    def getVHDLVSize(virtSize):
        return lvhdutil.calcSizeVHDLV(virtSize)
    getVHDLVSize = staticmethod(getVHDLVSize)

    def getSizeLV(size):
        """ Rounds up the size."""
        return util.roundup(lvutil.LVM_SIZE_INCREMENT, size)
    getSizeLV = staticmethod(getSizeLV)

    def __init__(self, logger):
        self.session = cleanup.XAPI.getSession()
        self.logger = logger
        self.targetDevice = None
        self.createdSRs = []

        # List of VDIs that have been created, but not yet deleted.
        self.createdVDIs = []

        self.createdVBDs = {}
        self.devices = {}
        self.nextVDINumber = 0
        self.usedVBDLetters = {}
        self.availableVBDLetters = []
        self.srTypes = {}
        for c in range(ord(self.FIRST_VBD_LETTER), ord(self.LAST_VBD_LETTER)):
            self.availableVBDLetters.append(chr(c))

    def getInfoSR(self, uuid):
        """Retrieves the parameters of the specified SR in the form of a
        dictionary."""

        return self._getInfoSR(uuid)

    def getDefaultSR(self, sm):
        """Retrieves the UUID of the default SR. Insert the SR UUID/type pair
        into to srTypes member dictionary."""
        # TODO why pass a different StorageManager instance?

        sr = self._getDefaultSR()
        self.srTypes[sr] = sm.getInfoSR(sr)["type"]
        return sr

    def refreshSR(self, sr):
        """"Refreshes" the list of SRs."""
        self._refreshSR(sr)

    def createSR(self, type, size, name):
        if (not type in self.STORAGE_TYPE_SR_NAME.keys()):
            raise SMException("unknown SR type: %d" % type)
        self.logger.log("Creating SR of type %s..." %\
                self.STORAGE_TYPE_NAME[type], 2)
        sr = self._createSR(self.STORAGE_TYPE_SR_NAME[type], size, name)
        self.createdSRs.append(sr)
        return sr

    def getParam(self, obj, uuid, param):
        """Always return a dictionary"""
        val = self._toDict(self._getParam(obj, uuid, param))[param]
        if isinstance(val, str):
            return {val: ""} # FIXME what's this?
        return val
    
    def vdi_get_param(self, uuid, param):
        return self.getParam('vdi', uuid, param)

    def vdi_has_caching_enabled(self, uuid):
        """Returns True if caching has been enabled on the specified VDI, False
        otherwise."""
        return 'true' in self.vdi_get_param(uuid, 'allow-caching')

    def vbd_is_attached(self, uuid):
        return 'true' in self.vbd_get_param(uuid, 'currently-attached')

    def setParam(self, obj, uuid, param, val, set = True):
        self._setParam(obj, uuid, param, val, set)

    def probe(self, sr, dev):
        return self._probe(self.srTypes[sr], dev)

    def findSR(self, type):
        """Retrieves SRs of the specified type in the form of a list. Each SR
        UUID/type pair is inserted into the srTypes member dictionary."""

        srList = self._findSR(type)
        for sr in srList:
            self.srTypes[sr] = type
        return srList

    def findThisPBD(self, sr):
        """Find the PBD for SR "sr" for this host"""
        return self._findThisPBD(sr)

    def plugSR(self, sr):
        return self._plugSR(sr)

    def unplugSR(self, sr):
        return self._unplugSR(sr)

    def getVDIs(self, sr):
        """Retrieves the UUIDs of all the VDIs on the specified SR, in the form
        of a list."""

        return self._getVDIs(sr)

    def getLeafVDIs(self, sr):
        """Retrieves the UUIDs of the leaf VDIs on the specified SR in the form
        of a list."""

        return self._getLeafVDIs(sr)

    def getInfoPBD(self, uuid):
        return self._getInfoPBD(uuid)

    def getInfoVDI(self, uuid):
        """Retrieves the parameters of the spcified VDI in the form of a
        dictionary."""

        return self._getInfoVDI(uuid)

    def learnVDI(self, vdi):
        self.createdVDIs.append(vdi)

    def unlearnVDI(self, vdi):
        if not vdi in self.createdVDIs:
            raise SMException("Don't know VDI %s" % vdi)
        self.createdVDIs.remove(vdi)

    def createVDI(self, sr, size, raw = False):
        """Creates a VDI with the specified size, on the specified SR."""

        name = "v"
        if raw:
            name = "r"
        name = "%s%d" % (name, self.nextVDINumber)
        self.nextVDINumber += 1

        self.logger.log("Creating VDI %s..." % name, 2)
        vdi = self._createVDI(sr, size, name, raw)
        self.createdVDIs.append(vdi)
        return vdi

    def cloneVDI(self, vdi):
        if not vdi in self.createdVDIs:
            raise SMException("Don't know VDI %s" % vdi)
        self.logger.log("Cloning VDI...", 2)
        clone = self._cloneVDI(vdi)
        self.createdVDIs.append(clone)
        return clone

    def snapshotVDI(self, vdi, single = False):
        if not vdi in self.createdVDIs:
            raise SMException("Don't know VDI %s" % vdi)
        self.logger.log("Snapshotting VDI...", 2)
        snap = self._snapshotVDI(vdi, single)
        if not single:
            self.createdVDIs.append(snap)
        return snap

    def resizeVDI(self, vdi, size, live):
        self._resizeVDI(vdi, size, live)

    def getPathSR(self, sr):
        """Retrieves the path on dom0 of the specified SR if it is of NFS or
        ext3 type, otherwise the block device on dom0."""

        if self.srTypes[sr] in ["ext", "nfs"]:
            return "/var/run/sr-mount/%s" % sr
        else:
            return "/dev/%s%s" % (self.VG_PREFIX, sr)

    def getFileVDI(self, sr, vdi):
        if self.srTypes[sr] in ["ext", "nfs"]:
            return "%s.vhd" % vdi
        else:
            return "VHD-%s" % vdi

    def getPathVDI(self, sr, vdi):
        srPath = self.getPathSR(sr)
        return os.path.join(srPath, self.getFileVDI(sr, vdi))

    def getParentVDI(self, sr, vdi):
        if self.srTypes[sr] in ["ext", "nfs"]:
            return vhdutil.getParent(self.getPathVDI(sr, vdi), FileSR.FileVDI.extractUuid).strip()
        else:
            return vhdutil.getParent(self.getPathVDI(sr, vdi), lvhdutil.extractUuid).strip()

    def getInfoVBD(self, uuid):
        return self._getInfoVBD(uuid)

    def createVBD(self, vdi, ro, vm = None):
        if (not vdi in self.createdVDIs):
            raise SMException("Don't know VDI %s" % vdi)
        if self.createdVBDs.get(vdi):
            raise SMException("VDI %s already has VBD %s" % \
                    (vdi, self.createdVBDs[vdi]))
        if len(self.availableVBDLetters) == 0:
            raise SMException("Out of dev names")
        devLetter = self.availableVBDLetters.pop()
        if not vm:
            vm = self._getDom0UUID()
        try:
            vbd = self._createVBD(vdi, ro, vm, devLetter)
        except (tutil.CommandException, SMException):
            self.availableVBDLetters.append(devLetter)
            raise
        self.createdVBDs[vdi] = vbd
        self.usedVBDLetters[vbd] = devLetter

    def plugVBD(self, vdi):
        if not self.createdVBDs.get(vdi):
            raise SMException("Don't have VBD for %s" % vdi)
        vbd = self.createdVBDs[vdi]
        device = self._plugVBD(vbd)
        self.devices[vbd] = device

    def unplugVBD(self, vdi):
        if not self.createdVBDs.get(vdi):
            raise SMException("Don't have VBD for %s" % vdi)
        vbd = self.createdVBDs[vdi]            
        self._unplugVBD(vbd)
        del self.devices[vbd]

    def destroyVBD(self, vdi):
        if not self.createdVBDs.get(vdi):
            raise SMException("Don't have VBD for %s" % vdi)
        vbd = self.createdVBDs[vdi]
        self._destroyVBD(vbd)
        del self.createdVBDs[vdi]
        self.availableVBDLetters.append(self.usedVBDLetters[vbd])
        del self.usedVBDLetters[vbd]

    def getDevice(self, vdi):
        return self.devices[self.createdVBDs[vdi]]

    def plugVDI(self, vdi, ro, vm = None):
        """Creates and plugs a VBD, effectively attaching the specified VDI."""

        self.createVBD(vdi, ro, vm)
        self.plugVBD(vdi)

    def _vdi_attach(self, vdi_uuid, vm_uuid, vbd_uuid = None):
        """Attaches the VDI to the VM using the specified VBD. If no VBD is
        supplied it is created. The UUID of the VBD (either it was created or
        not) and the device path are returned."""
        if None == vbd_uuid:
            vbd_uuid = self._createVBD(vdi_uuid, vm_uuid)
        return (vbd_uuid, self._plugVBD(vbd_uuid))


    def unplugVDI(self, vdi):
        self.unplugVBD(vdi)
        self.destroyVBD(vdi)

    def destroyVDI(self, vdi):
        self.unlearnVDI(vdi)
        self._destroyVDI(vdi)

    def vdi_get_vbd(vdi_uuid):
        """Retrieves the UUID of the specified VDI, else None."""
        return self._vdi_get_vbd(vdi_uuid)

    def destroySR(self, sr):
        if (not sr in self.createdSRs):
            raise SMException("Don't know SR %s" % sr)
        self._destroySR(sr)
        self.createdSRs.remove(sr)

    def unplugAll(self):
        self.logger.log("unplugAll: %d VBDs" % len(self.createdVBDs), 4)
        vdis = self.createdVBDs.keys()
        for vdi in vdis:
            try:
                self.unplugVBD(vdi)
            except SMException:
                pass
            self.destroyVBD(vdi)

    def destroyAll(self):
        """VDIs must not be plugged"""
        self.logger.log("destroyAll:  SR: %s" % " ".join(self.createdSRs), 4)
        self.logger.log("destroyAll: VDI: %s" % " ".join(self.createdVDIs), 4)
        while len(self.createdVDIs) > 0:
            self.destroyVDI(self.createdVDIs[0])
        while len(self.createdSRs) > 0:
            self.destroySR(self.createdSRs[0])

    def getDeviceName(self, vbd):
        letter = self._getVBDLetter(vbd)
        if letter:
            device = "/dev/xvd%s" % letter
            try:
                mode = os.stat(device).st_mode
                if stat.S_ISBLK(mode):
                    return device
            except OSError:
                pass
        return None

    def mkfs(self, target):
        self.logger.log("Putting ext2 on %s" % target, 2)
        cmd = "mkfs -t ext2 %s" % target
        tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD, tutil.RET_RC)

    def mount(self, dev, mountDir):
        if not tutil.pathExists(mountDir):
            try:
                os.makedirs(mountDir)
            except OSError:
                raise SMException("makedirs failed (for %s)" % mountDir)
        if not tutil.pathExists(mountDir):
            raise SMException("mount dir '%s' not created" % mountDir)
        cmd = "mount %s %s" % (dev, mountDir)
        tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)

    def umount(self, mountDir):
        cmd = "umount %s" % mountDir
        tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)

    def activateChain(self, sr, vdi, parentOnly = True):
        """Activate the LV of the VHD 'vdi' and also all its VHD parents. 
        Return the list of all the LV's that were activated"""
        if self.srTypes[sr] in ["ext", "nfs"]:
            return
        activated = []
        gotParent = False
        lvName = self.getFileVDI(sr, vdi)
        path = self.getPathVDI(sr, vdi)
        lvmCache = lvmcache.LVMCache("%s%s" % (self.VG_PREFIX, sr))
        while True:
            activated.append((vdi, lvName))
            self.logger.log("Activating %s" % lvName, 4)
            lvmCache.activate("lvm-" + sr, vdi, lvName, False)
            # sometimes on the slave the device is still not created after 
            # lvchange -ay. We do a forceful refresh here just in case
            #cmd = "lvchange --refresh -ay %s" % path
            #tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_SUB)
            if parentOnly and gotParent:
                break
            path = self._getVHDParentNoCheck(sr, path)
            if not path:
                break
            lvName = path.split('/')[-1].replace("--", "-")
            vdi = lvName.split("-", 1)[-1]
            self.logger.log("VHD parent = %s" % vdi, 4)
            gotParent = True
        return activated

    def deactivateVDIs(self, sr, vdis):
        if self.srTypes[sr] in ["ext", "nfs"]:
            return
        lvmCache = lvmcache.LVMCache("%s%s" % (self.VG_PREFIX, sr))
        for (vdi, lvName) in vdis:
            self.logger.log("Deactivating %s" % lvName, 4)
            lvmCache.deactivate("lvm-" + sr, vdi, lvName, False)

    def getVMs(self):
        """Get a pair of dom0 VMs: one for master, one for a slave (None
        if this is not a pool)"""
        return self._getVMs()

    def getMasterUUID(self):
        return self._getMasterUUID()

    def getThisHost(self):
        "Get the host uuid for the host we're running on"
        return self._getThisHost()

    def getThisDom0(self):
        "Get the dom0 VM uuid for the host we're running on"
        return self._getThisDom0()

    def onMaster(self, plugin, fn, args):
        return self._onMaster(plugin, fn, args)

    def _getVBDLetter(self, vbd):
        if self.usedVBDLetters.has_key(vbd):
            return self.usedVBDLetters[vbd]
        else:
            return None

    def _getVHDParentNoCheck(self, sr, path):
        cmd = "vhd-util read -p -n %s" % path
        text = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_SUB)
        for line in text.split('\n'):
            if line.find("decoded name :") != -1:
                val = line.split(':')[1].strip()
                vdi = val.replace("--", "-")[-40:]
                if vdi[1:].startswith("LV-"):
                    vdi = vdi[1:]
                return os.path.join(self.getPathSR(sr), vdi)
        return None

    def _toDict(strInfo):
        info = dict()
        for line in strInfo.split('\n'):
            if not line.strip():
                continue
            key, val = line.split(':', 1)
            key = key.strip()
            val = val.strip()
            m = re.match(".*\([ MS]R[WO]\).*", key)
            if m != None:
                key = key[0:-len(" ( RW)")]
            valmap = None
            if val.find(":") != -1:
                valmap = dict()
                for pair in val.split(";"):
                    key2 = pair.strip()
                    val2 = ""
                    if pair.find(":") != -1:
                        key2, val2 = pair.split(':', 1)
                        key2 = key2.strip()
                        val2 = val2.strip()
                    valmap[key2] = val2
            if valmap:
                info[key] = valmap
            else:
                info[key] = val
        return info
    _toDict = staticmethod(_toDict)



class StorageManagerXAPI(StorageManager):
    "Manage XenServer storage through XML-RPC"

    # not implemented
    def __init__(self, logger, address, user, password):
        StorageManager.__init__(self, logger)
        self.xen = xmlrpclib.Server(address)



class StorageManagerCLI(StorageManager):
    "Manage local XenServer storage through CLI"

    def __init__(self, logger):
        StorageManager.__init__(self, logger)

    def _getMasterUUID(self):
        cmd = "xe pool-list params=master --minimal"
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_SUB)
        master = stdout.strip()
        return master

    def _getDom0UUID(self):
        cmd = "xe vm-list dom-id=0 params=uuid --minimal"
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        if not stdout:
            raise SMException("No output from vm-list")
        dom0uuid = stdout.strip()
        if not tutil.validateUUID(dom0uuid):
            raise SMException("Got invalid UUID: %s" % dom0uuid)
        return dom0uuid

    def _getPoolUUID(self):
        cmd = "xe pool-list params=uuid --minimal"
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        poolUuid = stdout.strip()
        if not tutil.validateUUID(poolUuid):
            raise SMException("Got invalid UUID: %s" % poolUuid)
        return poolUuid

    def _getDefaultSR(self):
        """Retrieves the UUID of the default SR of the pool."""

        cmd = "xe pool-param-get param-name=default-SR uuid=%s --minimal" % \
                self._getPoolUUID()
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        uuid = stdout.strip()
        if tutil.validateUUID(uuid):
            return uuid
        return None

    def _getInfoSR(self, uuid):
        """Retrieves the parameters of the specified SR in the form of a
        dictionary."""

        cmd = "xe sr-list uuid=%s params=all" % uuid
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD + 1)
        info = self._toDict(stdout)
        if not info["sm-config"]:
            info["sm-config"] = dict()
        return info

    def _refreshSR(self, sr):
        """"Refreshes" the list of SRs by performing an SR scan."""
        # TODO What is the use of the refresh?
        cmd = "xe sr-scan uuid=%s" % sr
        tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD + 1)

    def _getInfoPBD(self, uuid):
        cmd = "xe pbd-list uuid=%s params=all" % uuid
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD + 1)
        return self._toDict(stdout)

    def _getInfoVDI(self, uuid):
        """Retrieves the parameters of the spcified VDI in the form of a
        dictionary."""

        cmd = "xe vdi-list uuid=%s params=all" % uuid
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD + 1)
        return self._toDict(stdout)

    def _createSR(self, type, size):
        cmd = "xe sr-create name-label='%s' physical-size=%d \
                content-type=user device-config:device=%s type=%s" % \
                (self.SR_LABEL, size, self.targetDevice, type)
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        if not stdout:
            raise SMException("No output from sr-create")
        srUuid = stdout.strip()
        if not tutil.validateUUID(srUuid):
            raise SMException("Got invalid UUID: %s" % srUuid)
        return srUuid

    def _getParam(self, obj, uuid, param):
        """Retrieves the specified parameter(s) of the specified object.

        Arguments:

        object: An object can be anything that would render the "<obj>-list"
        string valid for passing it as an argument to a "xe" command, e.g.
        sr-list, vdi-list etc. The uuid is the UUID of the desired object.

        uuid: The UUID of the specified object.

        param: The desired parameter, e.g. all, allow-caching, etc."""

        cmd = "xe %s-list uuid=%s params=%s" % (obj, uuid, param)
        return tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_SUB)

    def _setParam(self, obj, uuid, param, val, set = True):
        cmd = "xe %s-param-set uuid=%s %s=%s" % (obj, uuid, param, val)
        if not set:
            cmd = "xe %s-param-clear uuid=%s param-name=%s" % (obj, uuid, param)
        tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)

    def _probe(self, type, dev):
        cmd = "xe sr-probe type=%s device-config:device=%s" % (type, dev)
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        return stdout

    def _findSR(self, type):
        "Retrieves all SRs of the specified type in the form of a list."

        cmd = "xe sr-list type=%s --minimal" % type
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        stdout = stdout.strip()
        if not stdout:
            return []
        srList = stdout.split(",")
        return srList

    def _findThisPBD(self, sr):
        """Retrieves the UUID of the PDB of the specified SR."""
        cmd = "xe pbd-list sr-uuid=%s host-uuid=%s params=uuid --minimal" % \
                (sr, self.getThisHost())
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        return stdout.strip()

    def _plugSR(self, sr):
        cmd = "xe pbd-list sr-uuid=%s params=uuid --minimal" % sr
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        for uuid in stdout.split(","):
            cmd = "xe pbd-plug uuid=%s" % uuid
            tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)

    def _unplugSR(self, sr):
        cmd = "xe pbd-list sr-uuid=%s params=uuid --minimal" % sr
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        for uuid in stdout.split(","):
            cmd = "xe pbd-unplug uuid=%s" % uuid
            tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)

    def _getVDIs(self, sr):
        """Retrieves the UUIDs of all the VDIs on the specified SR, in the form
        of a list."""

        cmd = "xe vdi-list sr-uuid=%s params=uuid --minimal" % sr
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_SUB)
        stdout = stdout.strip()
        if not stdout:
            return []
        vdiList = stdout.split(",")
        return vdiList

    def _getLeafVDIs(self, sr):
        """Retrieves the UUIDs of the leaf VDIs on the specified SR in the form
        of a list."""

        vdiList = self.getVDIs(sr)
        cmd = "xe vdi-list sr-uuid=%s name-label='base copy' params=uuid --minimal" % sr
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_SUB)
        stdout = stdout.strip()
        if not stdout:
            baseList = []
        baseList = stdout.split(",")

        # TODO Which VDIs does the name-label='base copy' return?
        leafList = []
        for vdi in vdiList:
            if vdi not in baseList:
                leafList.append(vdi)
        return leafList

    # TODO Make name an optional parameter where if not specified, a random
    # one should be generated. XXX "a random one" what?
    def _createVDI(self, sr, size = 2**30, name = None, raw = False):
        """Creates a VDI on the specified SR using the specified size and name.
        The raw argument controls whether the VDI to be created must be of raw
        or VHD format. Returns the UUID of the created VDI."""

        if None == name:
            name = ''.join(random.choice(string.hexdigits) for x in range(3))      
        cmd = "xe vdi-create sr-uuid=%s name-label='%s' \
                type=user virtual-size=%d name-description=test-vdi" \
                % (sr, name, size)
        if raw:
            cmd += " sm-config:type=raw"
        else:
            cmd += " sm-config:type=vhd"
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        if not stdout:
            raise SMException("No output from vdi-create")
        vdiUuid = stdout.strip()
        if not tutil.validateUUID(vdiUuid):
            raise SMException("Got invalid UUID: %s" % vdiUuid)
        return vdiUuid

    def _cloneVDI(self, vdi):
        cmd = "xe vdi-clone uuid=%s" % vdi
        for i in range(CMD_NUM_RETRIES):
            try:
                stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
                break
            except tutil.CommandException, inst:
                if str(inst).find("VDI_IN_USE") != -1:
                    self.logger.log("Command failed, retrying", LOG_LEVEL_CMD)
                    time.sleep(CMD_RETRY_PERIOD)
                else:
                    raise

        if not stdout:
            raise SMException("No output from vdi-snapshot")
        cloneUuid = stdout.strip()
        if not tutil.validateUUID(cloneUuid):
            raise SMException("Got invalid UUID: %s" % cloneUuid)
        return cloneUuid

    def _snapshotVDI(self, vdi, single = None):
        cmd = "xe vdi-snapshot uuid=%s" % vdi
        if single:
            cmd += " driver-params:type=single"
        for i in range(CMD_NUM_RETRIES):
            try:
                stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
                break
            except tutil.CommandException, inst:
                if str(inst).find("VDI_IN_USE") != -1:
                    self.logger.log("Command failed, retrying", LOG_LEVEL_CMD)
                    time.sleep(CMD_RETRY_PERIOD)
                else:
                    raise

        if not stdout:
            raise SMException("No output from vdi-snapshot")
        snapUuid = stdout.strip()
        if not tutil.validateUUID(snapUuid):
            raise SMException("Got invalid UUID: %s" % snapUuid)
        return snapUuid

    def _resizeVDI(self, vdi, size, live):
        cmd = "xe vdi-resize uuid=%s disk-size=%d" % (vdi, size)
        if live:
            cmd += " online=true"
        tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)

    def _getInfoVBD(self, uuid):
        cmd = "xe vbd-list uuid=%s params=all" % uuid
        stdout = tutil.execCmd(cmd, 0, self.logger, 4)
        return self._toDict(stdout)

    def _createVBD(self, vdi, vm, ro = False, vbdLetter = None,
            unpluggable = True):
        """Creates a VBD for the specified VDI on the specified VM. If a device
        is not supplied (vbdLetter), the first available one is used. Returns
        the UUID of the VBD."""

        mode = "rw"
        if ro:
            mode="ro"

        if None == vbdLetter:
            devices = self._vm_get_allowed_vbd_devices(vm)
            assert len(devices) > 0 # FIXME raise exception instead
            vbdLetter = devices[0]
        
        cmd = "xe vbd-create vm-uuid=%s vdi-uuid=%s type=disk mode=%s "\
                "device=%s unpluggable=%s" % (vm, vdi, mode, vbdLetter,
                        unpluggable)
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        # XXX xe vbd-create returns 0 if the device name is invalid
        if not stdout:
            raise SMException("No output from vbd-create")
        vbdUuid = stdout.strip()
        if not tutil.validateUUID(vbdUuid):
            raise SMException("Got invalid UUID: %s" % vbdUuid)
        return vbdUuid

    def _plugVBD(self, vbd):
        """Plugs the VBD and returns the device name."""
        self.logger.log("Plugging VBD %s" % vbd, 2)
        cmd = "xe vbd-plug uuid=" + vbd
        tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        return self.getDeviceName(vbd)

    def _unplugVBD(self, vbd):
        self.logger.log("Unplugging VBD %s" % vbd, 2)
        cmd = "xe vbd-unplug uuid=%s" % vbd
        try:
            tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        except tutil.CommandException, inst:
            if str(inst).find("device is not currently attached") == -1:
                raise

    def _destroyVBD(self, vbd):
        self.logger.log("Destroying VBD %s" % vbd, 3)
        cmd = "xe vbd-destroy uuid=%s" % vbd
        tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)

    def _destroyVDI(self, vdi):
        self.logger.log("Destroying VDI %s" % vdi, 3)
        cmd = "xe vdi-destroy uuid=%s" % vdi
        tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)

    def _destroySR(self, sr):
        self.logger.log("Destroying SR %s" % sr, 2)
        cmd = "xe pbd-list sr-uuid=%s params=uuid" % sr
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        if not stdout:
            raise SMException("PBD not found for SR %s" % sr)
        pbdUuid = stdout.strip().split(":")[1].strip()
        cmd = "xe pbd-unplug uuid=%s" % pbdUuid
        tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)

        cmd = "xe sr-destroy uuid=%s" % sr
        tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)

        cmd = "xe sr-list uuid=%s" % sr
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        if stdout:
            raise SMException("SR %s still present after sr-destroy" % sr)

    def _getVMs(self):
        master = self.getMasterUUID()
        cmd = "xe vm-list dom-id=0 resident-on=%s params=uuid --minimal" % \
                master
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD + 1)
        masterVM = stdout.strip()
        if not tutil.validateUUID(masterVM):
            raise SMException("Got invalid UUID: %s" % masterVM)

        slaveVM = None
        host = self.getThisHost()
        if host == master:
            cmd = "xe host-list params=uuid --minimal"
            stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD + 1)
            hosts = stdout.strip().split(",")
            for h in hosts:
                if h != master:
                    host = h
                    break

        if host == master:
            return (masterVM, None)

        cmd = "xe vm-list dom-id=0 resident-on=%s params=uuid --minimal" % host
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD + 1)
        slaveVM = stdout.strip()
        if not tutil.validateUUID(slaveVM):
            raise SMException("Got invalid UUID: %s" % slaveVM)
        return (masterVM, slaveVM)

    def _getThisHost(self):
        uuid = None
        f = open("/etc/xensource-inventory", 'r')
        for line in f.readlines():
            if line.startswith("INSTALLATION_UUID"):
                uuid = line.split("'")[1]
        f.close()
        return uuid

    def _getThisDom0(self):
        uuid = None
        f = open("/etc/xensource-inventory", 'r')
        for line in f.readlines():
            if line.startswith("CONTROL_DOMAIN_UUID"):
                uuid = line.split("'")[1]
        f.close()
        return uuid

    def _onMaster(self, plugin, fn, args):
        argsStr = ""
        for key, val in args.iteritems():
            argsStr += " args:%s=%s" % (key, val)
        cmd = "xe host-call-plugin host-uuid=%s plugin=%s fn=%s %s" % \
                (self.getMasterUUID(), plugin, fn, argsStr)
        stdout = tutil.execCmd(cmd, 0, self.logger, LOG_LEVEL_CMD)
        return stdout.strip()

    def _vdi_get_vbd(self, vdi_uuid):
        """Retrieves the UUID of the specified VDI, else None."""
        out = tutil.execCmd('xe vbd-list vdi-uuid=' + vdi_uuid + ' --minimal',
                0, self.logger, LOG_LEVEL_CMD).strip()
        if '' != out:
            return out
        else:
            return None

    def _vm_get_param(self, vm_uuid, param_name):
        return tutil.execCmd('xe vm-param-get uuid=' + vm_uuid + \
                ' param-name=' + param_name, 0, self.logger, LOG_LEVEL_CMD)

    def _vm_get_allowed_vbd_devices(self, vm_uuid):
        """Returns a list of the allowed VBD devices for the specified VM."""

        devices = self._vm_get_param(vm_uuid, 'allowed-VBD-devices').split(';')
        for device in devices:
            device = device.strip()
        return devices

    def _vbd_get_param(self, vbd_uuid, param):
        return tutil.execCmd('xe vbd-param-get uuid=' + vbd_uuid + \
                ' param-name=' + param, 0, self.logger, LOG_LEVEL_CMD).rstrip()

    def _vdi_set_param(self, vdi_uuid, param, val):
        return self._setParam('vdi', vdi_uuid, param, val)

    def _vdi_get_param(self, vdi_uuid, param):
        return tutil.execCmd('xe vdi-param-get uuid=' + vdi_uuid \
                + ' param-name=' + param + ' --minimal', 0, self.logger, \
                LOG_LEVEL_CMD).rstrip()

    def _vdi_enable_caching(self, vdi_uuid, persistent = None):
        rc = self._vdi_set_param(vdi_uuid, 'allow-caching', 'true')
        if 0 != rc:
            return rc
        if None != persistent:
            return self._vdi_cache_persistence(vdi_uuid, persistent)
        else:
            return rc

    def _vdi_disable_caching(self, vdi_uuid):
        return self._vdi_set_param(vdi_uuid, 'allow-caching', 'false')
    
    def _vm_shutdown(self, vm_uuid):
        tutil.execCmd('xe vm-shutdown uuid=' + vm_uuid, 0, self.logger,
                LOG_LEVEL_CMD)

    def _vm_start(self, vm_uuid):
        tutil.execCmd('xe vm-start uuid=' + vm_uuid, 0, self.logger,
                LOG_LEVEL_CMD)

    def _vbd_get_bdev(self, vbd_uuid):
        """Retrieves the block device that represents this VBD."""

        return self._vbd_get_param(vbd_uuid, 'device')

    def _vdi_cache_persistence(self, vdi_uuid, enable = None):
        """If the enable argument is not specified, it tells whether caching on
        the specified VDI is persistent. Otherwise, the enable argument
        controls cache persistence."""

        if None == enable:
            val = self._vdi_get_param(vdi_uuid, 'on-boot').rstrip()
            if (not 'persist' == val) and (not 'reset' == val):
                raise Exception('unexpected persistence mode \'' + val + '\'')
            return 'persist' == val
        else:            
            assert True == enable or False == enable
            if enable:
                val = 'persist'
            else:
                val = 'reset'
            self._vdi_set_param(vdi_uuid, 'on-boot', val)

    def _vm_is_running(self, vm_uuid):
        return 'running' == self._vm_get_param(vm_uuid, 'power-state').rstrip()

    def _host_getParam(self, uuid, param):
        return tutil.execCmd('xe host-param-get uuid=' + uuid \
                + ' param-name=' + param + ' --minimal', 0, self.logger,
                LOG_LEVEL_CMD)

    def _host_get_local_cache_sr(self, uuid = None):
        if None == uuid:
            uuid = self._getThisHost()
        return self._host_getParam(uuid, 'local-cache-sr').rstrip()

    def _vdi_get_parent(self, vdi_uuid):
        """Retrieves the parent UUID of the VDI."""
        # XXX It is expected to be a VHD VDI.

        out = tutil.execCmd('xe vdi-param-get uuid=' + vdi_uuid \
                + ' param-name=sm-config', 0, self.logger,
                LOG_LEVEL_CMD)

        assert re.match('vhd-parent: ' + tutil.uuidre + '; vdi_type: vhd\n',
                out)

        return re.search(tutil.uuidre, out).group(0)
