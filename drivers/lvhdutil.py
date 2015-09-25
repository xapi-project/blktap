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

"""Helper functions for LVHD SR. This module knows about RAW and VHD VDI's 
that live in LV's."""


import os
import sys
import time

import util
import vhdutil
import xs_errors
from lock import Lock
from refcounter import RefCounter


MSIZE_MB = 2 * 1024 * 1024 # max virt size for fast resize
MSIZE = long(MSIZE_MB * 1024 * 1024)

VG_LOCATION = "/dev"
VG_PREFIX = "VG_XenStorage-"
LVM_SIZE_INCREMENT = 4 * 1024 * 1024

LV_PREFIX = { 
        vhdutil.VDI_TYPE_VHD : "VHD-",
        vhdutil.VDI_TYPE_RAW : "LV-",
}
VDI_TYPES = [ vhdutil.VDI_TYPE_VHD, vhdutil.VDI_TYPE_RAW ]

JRN_INFLATE = "inflate"

JVHD_TAG = "jvhd"

LOCK_RETRY_ATTEMPTS = 20

# ref counting for VDI's: we need a ref count for LV activation/deactivation
# on the master
NS_PREFIX_LVM = "lvm-"

class VDIInfo:
    uuid       = ""
    scanError  = False
    vdiType    = None
    lvName     = ""
    sizeLV     = -1
    sizeVirt   = -1
    lvActive   = False
    lvOpen     = False
    lvReadonly = False
    hidden     = False
    parentUuid = ""

    def __init__(self, uuid):
        self.uuid = uuid


def matchLV(lvName):
    """given LV name, return the VDI type and the UUID, or (None, None)
    if the name doesn't match any known type"""
    for vdiType in VDI_TYPES:
        prefix = LV_PREFIX[vdiType]
        if lvName.startswith(prefix):
            return (vdiType, lvName.replace(prefix, ""))
    return (None, None)

def extractUuid(path):
    uuid = os.path.basename(path)
    if uuid.startswith(VG_PREFIX):
        # we are dealing with realpath
        uuid = uuid.replace("--", "-")
        uuid.replace(VG_PREFIX, "")
    for t in VDI_TYPES:
        if uuid.find(LV_PREFIX[t]) != -1:
            uuid = uuid.split(LV_PREFIX[t])[-1]
            uuid = uuid.strip()
            # TODO: validate UUID format
            return uuid
    return None

def calcSizeLV(sizeVHD):
    return util.roundup(LVM_SIZE_INCREMENT, sizeVHD)

def calcSizeVHDLV(sizeVirt):
    # all LVHD VDIs have the metadata area preallocated for the maximum 
    # possible virtual size (for fast online VDI.resize)
    metaOverhead = vhdutil.calcOverheadEmpty(MSIZE)
    bitmapOverhead = vhdutil.calcOverheadBitmap(sizeVirt)
    return calcSizeLV(sizeVirt + metaOverhead + bitmapOverhead)

def getLVInfo(lvmCache, lvName = None):
    """Load LV info for all LVs in the VG or an individual LV. 
    This is a wrapper for lvutil.getLVInfo that filters out LV's that
    are not LVHD VDI's and adds the vdi_type information"""
    allLVs = lvmCache.getLVInfo(lvName)

    lvs = dict()
    for lvName, lv in allLVs.iteritems():
        vdiType, uuid = matchLV(lvName)
        if not vdiType:
            continue
        lv.vdiType = vdiType
        lvs[uuid] = lv
    return lvs

def getVDIInfo(lvmCache):
    """Load VDI info (both LV and if the VDI is not raw, VHD info)"""
    vdis = {}
    lvs = getLVInfo(lvmCache)

    haveVHDs = False
    for uuid, lvInfo in lvs.iteritems():
        if lvInfo.vdiType == vhdutil.VDI_TYPE_VHD:
            haveVHDs = True
        vdiInfo = VDIInfo(uuid)
        vdiInfo.vdiType    = lvInfo.vdiType
        vdiInfo.lvName     = lvInfo.name
        vdiInfo.sizeLV     = lvInfo.size
        vdiInfo.sizeVirt   = lvInfo.size
        vdiInfo.lvActive   = lvInfo.active
        vdiInfo.lvOpen     = lvInfo.open
        vdiInfo.lvReadonly = lvInfo.readonly
        vdiInfo.hidden     = lvInfo.hidden
        vdis[uuid]         = vdiInfo

    if haveVHDs:
        pattern = "%s*" % LV_PREFIX[vhdutil.VDI_TYPE_VHD]
        vhds = vhdutil.getAllVHDs(pattern, extractUuid, lvmCache.vgName)
        uuids = vdis.keys()
        for uuid in uuids:
            vdi = vdis[uuid]
            if vdi.vdiType == vhdutil.VDI_TYPE_VHD:
                if not vhds.get(uuid):
                    lvmCache.refresh()
                    if lvmCache.checkLV(vdi.lvName):
                        util.SMlog("*** VHD info missing: %s" % uuid)
                        vdis[uuid].scanError = True
                    else:
                        util.SMlog("LV disappeared since last scan: %s" % uuid)
                        del vdis[uuid]
                elif vhds[uuid].error:
                    util.SMlog("*** vhd-scan error: %s" % uuid)
                    vdis[uuid].scanError = True
                else:
                    vdis[uuid].sizeVirt   = vhds[uuid].sizeVirt
                    vdis[uuid].parentUuid = vhds[uuid].parentUuid
                    vdis[uuid].hidden     = vhds[uuid].hidden
    return vdis

def inflate(journaler, srUuid, vdiUuid, size):
    """Expand a VDI LV (and its VHD) to 'size'. If the LV is already bigger
    than that, it's a no-op. Does not change the virtual size of the VDI"""
    lvName = LV_PREFIX[vhdutil.VDI_TYPE_VHD] + vdiUuid
    vgName = VG_PREFIX + srUuid
    path = os.path.join(VG_LOCATION, vgName, lvName)
    lvmCache = journaler.lvmCache

    currSizeLV = lvmCache.getSize(lvName)
    newSize = calcSizeLV(size)
    if newSize <= currSizeLV:
        return
    journaler.create(JRN_INFLATE, vdiUuid, str(currSizeLV))
    util.fistpoint.activate("LVHDRT_inflate_after_create_journal",srUuid)
    lvmCache.setSize(lvName, newSize)
    util.fistpoint.activate("LVHDRT_inflate_after_setSize",srUuid)
    if not util.zeroOut(path, newSize - vhdutil.VHD_FOOTER_SIZE,
            vhdutil.VHD_FOOTER_SIZE):
        raise Exception('failed to zero out VHD footer')
    util.fistpoint.activate("LVHDRT_inflate_after_zeroOut",srUuid)
    vhdutil.setSizePhys(path, newSize, False)
    util.fistpoint.activate("LVHDRT_inflate_after_setSizePhys",srUuid)
    journaler.remove(JRN_INFLATE, vdiUuid)

def deflate(lvmCache, lvName, size):
    """Shrink the LV and the VHD on it to 'size'. Does not change the 
    virtual size of the VDI"""
    currSizeLV = lvmCache.getSize(lvName)
    newSize = calcSizeLV(size)
    if newSize >= currSizeLV:
        return
    path = os.path.join(VG_LOCATION, lvmCache.vgName, lvName)
    # no undo necessary if this fails at any point between now and the end
    vhdutil.setSizePhys(path, newSize)
    lvmCache.setSize(lvName, newSize)

def setSizeVirt(journaler, srUuid, vdiUuid, size, jFile):
    """When resizing the VHD virtual size, we might have to inflate the LV in
    case the metadata size increases"""
    lvName = LV_PREFIX[vhdutil.VDI_TYPE_VHD] + vdiUuid
    vgName = VG_PREFIX + srUuid
    path = os.path.join(VG_LOCATION, vgName, lvName)
    inflate(journaler, srUuid, vdiUuid, calcSizeVHDLV(size))
    vhdutil.setSizeVirt(path, size, jFile)

def _tryAcquire(lock):
    """We must give up if the SR is locked because it could be locked by the
    coalesce thread trying to acquire the VDI lock we're holding, so as to
    avoid deadlock"""
    for i in range(LOCK_RETRY_ATTEMPTS):
        gotLock = lock.acquireNoblock()
        if gotLock:
            return
        time.sleep(1)
    raise util.SRBusyException()

def attachThin(journaler, srUuid, vdiUuid):
    """Ensure that the VDI LV is expanded to the fully-allocated size"""
    lvName = LV_PREFIX[vhdutil.VDI_TYPE_VHD] + vdiUuid
    vgName = VG_PREFIX + srUuid
    lock = Lock(vhdutil.LOCK_TYPE_SR, srUuid)
    lvmCache = journaler.lvmCache
    _tryAcquire(lock)
    lvmCache.refresh()
    vhdInfo = vhdutil.getVHDInfoLVM(lvName, extractUuid, vgName)
    newSize = calcSizeVHDLV(vhdInfo.sizeVirt)
    currSizeLV = lvmCache.getSize(lvName)
    if newSize <= currSizeLV:
        return
    lvmCache.activate(NS_PREFIX_LVM + srUuid, vdiUuid, lvName, False)
    try:
        inflate(journaler, srUuid, vdiUuid, newSize)
    finally:
        lvmCache.deactivate(NS_PREFIX_LVM + srUuid, vdiUuid, lvName, False)
    lock.release()

def detachThin(session, lvmCache, srUuid, vdiUuid):
    """Shrink the VDI to the minimal size if no one is using it"""
    lvName = LV_PREFIX[vhdutil.VDI_TYPE_VHD] + vdiUuid
    path = os.path.join(VG_LOCATION, VG_PREFIX + srUuid, lvName)
    lock = Lock(vhdutil.LOCK_TYPE_SR, srUuid)
    _tryAcquire(lock)

    vdiRef = session.xenapi.VDI.get_by_uuid(vdiUuid)
    vbds = session.xenapi.VBD.get_all_records_where( \
            "field \"VDI\" = \"%s\"" % vdiRef)
    numPlugged = 0
    for vbdRec in vbds.values():
        if vbdRec["currently_attached"]:
            numPlugged += 1

    if numPlugged > 1:
        raise util.SMException("%s still in use by %d others" % \
                (vdiUuid, numPlugged - 1))
    lvmCache.activate(NS_PREFIX_LVM + srUuid, vdiUuid, lvName, False)
    try:
        newSize = calcSizeLV(vhdutil.getSizePhys(path))
        deflate(lvmCache, lvName, newSize)
    finally:
        lvmCache.deactivate(NS_PREFIX_LVM + srUuid, vdiUuid, lvName, False)
    lock.release()

def createVHDJournalLV(lvmCache, jName, size):
    """Create a LV to hold a VHD journal"""
    lvName = "%s_%s" % (JVHD_TAG, jName)
    lvmCache.create(lvName, size, JVHD_TAG)
    return os.path.join(lvmCache.vgPath, lvName)

def deleteVHDJournalLV(lvmCache, jName):
    """Delete a VHD journal LV"""
    lvName = "%s_%s" % (JVHD_TAG, jName)
    lvmCache.remove(lvName)

def getAllVHDJournals(lvmCache):
    """Get a list of all VHD journals in VG vgName as (jName,jFile) pairs"""
    journals = []
    lvList = lvmCache.getTagged(JVHD_TAG)
    for lvName in lvList:
        jName = lvName[len(JVHD_TAG) + 1:]
        journals.append((jName, lvName))
    return journals

def lvRefreshOnSlaves(session, srUuid, vgName, lvName, vdiUuid, slaves):
    args = {"vgName" : vgName,
            "action1": "activate",
            "uuid1"  : vdiUuid,
            "ns1"    : NS_PREFIX_LVM + srUuid,
            "lvName1": lvName,
            "action2": "refresh",
            "lvName2": lvName,
            "action3": "deactivate",
            "uuid3"  : vdiUuid,
            "ns3"    : NS_PREFIX_LVM + srUuid,
            "lvName3": lvName}
    for slave in slaves:
        util.SMlog("Refreshing %s on slave %s" % (lvName, slave))
        text = session.xenapi.host.call_plugin(slave, "on-slave", "multi", args)
        util.SMlog("call-plugin returned: '%s'" % text)

def lvRefreshOnAllSlaves(session, srUuid, vgName, lvName, vdiUuid):
    slaves = util.get_all_slaves(session)
    lvRefreshOnSlaves(session, srUuid, vgName, lvName, vdiUuid, slaves)

def setInnerNodeRefcounts(lvmCache, srUuid):
    """[Re]calculate and set the refcounts for inner VHD nodes based on
    refcounts of the leaf nodes. We can infer inner node refcounts on slaves
    directly because they are in use only when VDIs are attached - as opposed
    to the Master case where the coalesce process can also operate on inner
    nodes.
    Return all LVs (paths) that are active but not in use (i.e. that should
    be deactivated)"""
    vdiInfo = getVDIInfo(lvmCache)
    for uuid, vdi in vdiInfo.iteritems():
        vdi.refcount = 0

    ns = NS_PREFIX_LVM + srUuid
    for uuid, vdi in vdiInfo.iteritems():
        if vdi.hidden:
            continue # only read leaf refcounts
        refcount = RefCounter.check(uuid, ns)
        assert(refcount == (0, 0) or refcount == (0, 1))
        if refcount[1]:
            vdi.refcount = 1
            while vdi.parentUuid:
                vdi = vdiInfo[vdi.parentUuid]
                vdi.refcount += 1
    
    pathsNotInUse = []
    for uuid, vdi in vdiInfo.iteritems():
        if vdi.hidden:
            util.SMlog("Setting refcount for %s to %d" % (uuid, vdi.refcount))
            RefCounter.set(uuid, vdi.refcount, 0, ns)
        if vdi.refcount == 0 and vdi.lvActive:
            path = os.path.join("/dev", lvmCache.vgName, vdi.lvName)
            pathsNotInUse.append(path)

    return pathsNotInUse

if __name__ == "__main__":
    import lvutil
    # used by the master changeover script
    cmd = sys.argv[1]
    if cmd == "fixrefcounts":
        from lvmcache import LVMCache
        srUuid = sys.argv[2]
        try:
            vgName = VG_PREFIX + srUuid
            lvmCache = LVMCache(vgName)
            setInnerNodeRefcounts(lvmCache, srUuid)
        except:
            util.logException("setInnerNodeRefcounts")
    elif cmd == "extend":
        ssize = sys.argv[2]
        path = sys.argv[3]
        vgName = path.split('/')[-2].replace('VG_XenStorage', 'lvm')
        lvUuid = path.split('/')[-1].lstrip('VHD-')
        lock = Lock(vgName, lvUuid)
        util.SMlog("LV Extending %s to %s" % (path, ssize))
        _tryAcquire(lock)
        if not lvutil.extend(ssize, path):
            sys.exit(1)
        lock.release()
    else:
        util.SMlog("Invalid usage")
        print "Usage: %s <fixrefcounts|extend> ..." % sys.argv[0]
