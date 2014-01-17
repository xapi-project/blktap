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
# Miscellaneous LVM utility functions
#


import re
import os
import errno
import time

import SR
import util
import xs_errors
import xml.dom.minidom
from lvhdutil import VG_LOCATION,VG_PREFIX
import lvmcache
import srmetadata
import vhdutil

MDVOLUME_NAME = 'MGT'
VDI_UUID_TAG_PREFIX = 'vdi_'
LVM_BIN = os.path.isfile('/sbin/lvdisplay') and '/sbin' or '/usr/sbin'
CMD_VGS       = os.path.join(LVM_BIN, "vgs")
CMD_VGCREATE  = os.path.join(LVM_BIN, "vgcreate")
CMD_VGREMOVE  = os.path.join(LVM_BIN, "vgremove")
CMD_VGCHANGE  = os.path.join(LVM_BIN, "vgchange")
CMD_VGEXTEND  = os.path.join(LVM_BIN, "vgextend")
CMD_PVS       = os.path.join(LVM_BIN, "pvs")
CMD_PVCREATE  = os.path.join(LVM_BIN, "pvcreate")
CMD_PVREMOVE  = os.path.join(LVM_BIN, "pvremove")
CMD_PVRESIZE  = os.path.join(LVM_BIN, "pvresize")
CMD_LVS       = os.path.join(LVM_BIN, "lvs")
CMD_LVDISPLAY = os.path.join(LVM_BIN, "lvdisplay")
CMD_LVCREATE  = os.path.join(LVM_BIN, "lvcreate")
CMD_LVREMOVE  = os.path.join(LVM_BIN, "lvremove")
CMD_LVCHANGE  = os.path.join(LVM_BIN, "lvchange")
CMD_LVRENAME  = os.path.join(LVM_BIN, "lvrename")
CMD_LVRESIZE  = os.path.join(LVM_BIN, "lvresize")
CMD_DMSETUP   = "/sbin/dmsetup"

LVM_SIZE_INCREMENT = 4 * 1024 * 1024
LV_TAG_HIDDEN = "hidden"
LVM_FAIL_RETRIES = 10

MASTER_LVM_CONF = '/etc/lvm/master'
DEF_LVM_CONF = '/etc/lvm'

class LVInfo:
    name = ""
    size = 0
    active = False
    open = False
    hidden = False
    readonly = False

    def __init__(self, name):
        self.name = name

    def toString(self):
        return "%s, size=%d, active=%s, open=%s, hidden=%s, ro=%s" % \
                (self.name, self.size, self.active, self.open, self.hidden, \
                self.readonly)


def _checkVG(vgname):
    try:
        cmd = [CMD_VGS, vgname]
        util.pread2(cmd)
        return True
    except:
        return False

def _checkPV(pvname):
    try:
        cmd = [CMD_PVS, pvname]
        util.pread2(cmd)
        return True
    except:
        return False

def _checkLV(path):
    try:
        cmd = [CMD_LVDISPLAY, path]
        util.pread2(cmd)
        return True
    except:
        return False

def _getLVsize(path):
    try:
        cmd = [CMD_LVDISPLAY, "-c", path]
        lines = util.pread2(cmd).split(':')
        return long(lines[6]) * 512
    except:
        raise xs_errors.XenError('VDIUnavailable', \
              opterr='no such VDI %s' % path)

def _getVGstats(vgname):
    try:
        cmd = [CMD_VGS, "--noheadings", "--nosuffix", "--units", "b", vgname]
        text = util.pread(cmd).split()
        size = long(text[5])
        freespace = long(text[6])
        utilisation = size - freespace
        stats = {}
        stats['physical_size'] = size
        stats['physical_utilisation'] = utilisation
        stats['freespace'] = freespace
        return stats
    except util.CommandException, inst:
        raise xs_errors.XenError('VDILoad', \
              opterr='rvgstats failed error is %d' % inst.code)
    except ValueError:
        raise xs_errors.XenError('VDILoad', opterr='rvgstats failed')

def _getPVstats(dev):
    try:
        cmd = [CMD_PVS, "--noheadings", "--nosuffix", "--units", "b", dev]
        text = util.pread(cmd).split()
        size = long(text[4])
        freespace = long(text[5])
        utilisation = size - freespace
        stats = {}
        stats['physical_size'] = size
        stats['physical_utilisation'] = utilisation
        stats['freespace'] = freespace
        return stats
    except util.CommandException, inst:
        raise xs_errors.XenError('VDILoad', \
              opterr='pvstats failed error is %d' % inst.code)
    except ValueError:
        raise xs_errors.XenError('VDILoad', opterr='rvgstats failed')

# Retrieves the UUID of the SR that corresponds to the specified Physical
# Volume (pvname). Each element in prefix_list is checked whether it is a
# prefix of Volume Groups that correspond to the specified PV. If so, the
# prefix is stripped from the matched VG name and the remainder is returned
# (effectively the SR UUID). If no match if found, the empty string is
# returned.
# E.g.
#   PV         VG                          Fmt  Attr PSize   PFree  
#  /dev/sda4  VG_XenStorage-some-hex-value lvm2 a-   224.74G 223.73G
# will return "some-hex-value".
def _get_sr_uuid(pvname, prefix_list):
    try:
        cmd = [CMD_PVS, "--noheadings", "-o", "vg_name", pvname]
        return match_VG(util.pread2(cmd), prefix_list)
    except:
        return ""

# Tries to match any prefix contained in prefix_list in s. If matched, the
# remainder string is returned, else the empty string is returned. E.g. if s is
# "VG_XenStorage-some-hex-value" and prefix_list contains "VG_XenStorage-",
# "some-hex-value" is returned.
#
# TODO Items in prefix_list are expected to be found at the beginning of the
# target string, though if any of them is found inside it a match will be
# produced. This could be remedied by making the regular expression more
# specific.
def match_VG(s, prefix_list):
    for val in prefix_list:
        regex = re.compile(val)
        if regex.search(s, 0):
            return s.split(val)[1]
    return ""

# Retrieves the devices an SR is composed of. A dictionary is returned, indexed
# by the SR UUID, where each SR UUID is mapped to a comma-separated list of
# devices. Exceptions are ignored.
def scan_srlist(prefix, root):
    VGs = {}
    for dev in root.split(','):
        try:
            sr_uuid = _get_sr_uuid(dev, [prefix]).strip('\n')
            if len(sr_uuid):
                if VGs.has_key(sr_uuid):
                    VGs[sr_uuid] += ",%s" % dev
                else:
                    VGs[sr_uuid] = dev
        except Exception, e:
            util.logException("exception (ignored): %s" % e)
            continue
    return VGs

# Converts an SR list to an XML document with the following structure:
# <SRlist>
#   <SR>
#       <UUID>...</UUID>
#       <Devlist>...</Devlist>
#       <size>...</size>
#       <!-- If includeMetadata is set to True, the following additional nodes
#       are supplied. -->
#       <name_label>...</name_label>
#       <name_description>...</name_description>
#       <pool_metadata_detected>...</pool_metadata_detected>
#   </SR>
#
#   <SR>...</SR>
# </SRlist>
#
# Arguments:
#   VGs: a dictionary containing the SR UUID to device list mappings
#   prefix: the prefix that if prefixes the SR UUID the VG is produced
#   includeMetadata (optional): include additional information
def srlist_toxml(VGs, prefix, includeMetadata = False):
    dom = xml.dom.minidom.Document()
    element = dom.createElement("SRlist")
    dom.appendChild(element)
        
    for val in VGs:
        entry = dom.createElement('SR')
        element.appendChild(entry)

        subentry = dom.createElement("UUID")
        entry.appendChild(subentry)
        textnode = dom.createTextNode(val)
        subentry.appendChild(textnode)

        subentry = dom.createElement("Devlist")
        entry.appendChild(subentry)
        textnode = dom.createTextNode(VGs[val])
        subentry.appendChild(textnode)

        subentry = dom.createElement("size")
        entry.appendChild(subentry)
        size = str(_getVGstats(prefix + val)['physical_size'])
        textnode = dom.createTextNode(size)
        subentry.appendChild(textnode)
        
        if includeMetadata:
            metadataVDI = None
            
            # add SR name_label
            mdpath = os.path.join(VG_LOCATION, VG_PREFIX + val)
            mdpath = os.path.join(mdpath, MDVOLUME_NAME)
            try:
                mgtVolActivated = False
                if not os.path.exists(mdpath):
                    # probe happens out of band with attach so this volume
                    # may not have been activated at this point
                    lvmCache = lvmcache.LVMCache(VG_PREFIX + val)
                    lvmCache.activateNoRefcount(MDVOLUME_NAME)
                    mgtVolActivated = True
                
                sr_metadata = \
                    srmetadata.LVMMetadataHandler(mdpath, \
                                                        False).getMetadata()[0]
                subentry = dom.createElement("name_label")
                entry.appendChild(subentry)
                textnode = dom.createTextNode(sr_metadata[srmetadata.NAME_LABEL_TAG])
                subentry.appendChild(textnode)
                
                # add SR description
                subentry = dom.createElement("name_description")
                entry.appendChild(subentry)
                textnode = dom.createTextNode(sr_metadata[srmetadata.NAME_DESCRIPTION_TAG])
                subentry.appendChild(textnode)
                
                # add metadata VDI UUID
                metadataVDI = srmetadata.LVMMetadataHandler(mdpath, \
                                    False).findMetadataVDI()
                subentry = dom.createElement("pool_metadata_detected")
                entry.appendChild(subentry)
                if metadataVDI != None:
                    subentry.appendChild(dom.createTextNode("true"))
                else:
                    subentry.appendChild(dom.createTextNode("false"))
            finally:
                if mgtVolActivated:
                    # deactivate only if we activated it
                    lvmCache.deactivateNoRefcount(MDVOLUME_NAME)
                
    return dom.toprettyxml()

def createVG(root, vgname):
    systemroot = util.getrootdev()
    rootdev = root.split(',')[0]

    # Create PVs for each device
    for dev in root.split(','):
        if dev in [systemroot, '%s1' % systemroot, '%s2' % systemroot]:
            raise xs_errors.XenError('Rootdev', \
                  opterr=('Device %s contains core system files, ' \
                          + 'please use another device') % dev)
        if not os.path.exists(dev):
            raise xs_errors.XenError('InvalidDev', \
                  opterr=('Device %s does not exist') % dev)

        try:
            f = os.open("%s" % dev, os.O_RDWR | os.O_EXCL)
        except:
            raise xs_errors.XenError('SRInUse', \
                  opterr=('Device %s in use, please check your existing ' \
                  + 'SRs for an instance of this device') % dev)
        os.close(f)
        try:
            # Overwrite the disk header, try direct IO first
            cmd = [util.CMD_DD, "if=/dev/zero", "of=%s" % dev, "bs=1M",
                    "count=10", "oflag=direct"]
            util.pread2(cmd)
        except util.CommandException, inst:
            if inst.code == errno.EPERM:
                try:
                    # Overwrite the disk header, try normal IO
                    cmd = [util.CMD_DD, "if=/dev/zero", "of=%s" % dev,
                            "bs=1M", "count=10"]
                    util.pread2(cmd)
                except util.CommandException, inst:
                    raise xs_errors.XenError('LVMWrite', \
                          opterr='device %s' % dev)
            else:
                raise xs_errors.XenError('LVMWrite', \
                      opterr='device %s' % dev)
        try:
            cmd = [CMD_PVCREATE, "--metadatasize", "10M", dev]
            util.pread2(cmd)
        except util.CommandException, inst:
            raise xs_errors.XenError('LVMPartCreate', \
                  opterr='error is %d' % inst.code)

    # Create VG on first device
    try:
        cmd = [CMD_VGCREATE, vgname, rootdev]
        util.pread2(cmd)
    except :
        raise xs_errors.XenError('LVMGroupCreate')

    # Then add any additional devs into the VG
    for dev in root.split(',')[1:]:
        try:
            cmd = [CMD_VGEXTEND, vgname, dev]
            util.pread2(cmd)
        except util.CommandException, inst:
            # One of the PV args failed, delete SR
            try:
                cmd = [CMD_VGREMOVE, vgname]
                util.pread2(cmd)
            except:
                pass
            raise xs_errors.XenError('LVMGroupCreate')
    try:
        cmd = [CMD_VGCHANGE, "-an", vgname]
        util.pread2(cmd)
    except util.CommandException, inst:
        raise xs_errors.XenError('LVMUnMount', \
              opterr='errno is %d' % inst.code)

def removeVG(root, vgname):
    # Check PVs match VG
    try:
        for dev in root.split(','):
            cmd = [CMD_PVS, dev]
            txt = util.pread2(cmd)
            if txt.find(vgname) == -1:
                raise xs_errors.XenError('LVMNoVolume', \
                      opterr='volume is %s' % vgname)
    except util.CommandException, inst:
        raise xs_errors.XenError('PVSfailed', \
              opterr='error is %d' % inst.code)

    try:
        cmd = [CMD_VGREMOVE, vgname]
        util.pread2(cmd)

        for dev in root.split(','):
            cmd = [CMD_PVREMOVE, dev]
            util.pread2(cmd)
    except util.CommandException, inst:
        raise xs_errors.XenError('LVMDelete', \
              opterr='errno is %d' % inst.code)

def refreshPV(dev):
    try:
        cmd = [CMD_PVRESIZE, dev]
        util.pread2(cmd)
    except util.CommandException, inst:
        util.SMlog("Failed to grow the PV, non-fatal")
    
def setActiveVG(path, active):
    "activate or deactivate VG 'path'"
    val = "n"
    if active:
        val = "y"
    cmd = [CMD_VGCHANGE, "-a" + val, path]
    text = util.pread2(cmd)

def create(name, size, vgname, tag = None):
    size_mb = size / 1024 / 1024
    cmd = [CMD_LVCREATE, "-n", name, "-L", str(size_mb), vgname]
    if tag:
        cmd.extend(["--addtag", tag])
    util.pread2(cmd)

def remove(path):
    # see deactivateNoRefcount()
    for i in range(LVM_FAIL_RETRIES):
        try:
            _remove(path)
            break
        except util.CommandException, e:
            if i >= LVM_FAIL_RETRIES - 1:
                raise
            util.SMlog("*** lvremove failed on attempt #%d" % i)
    _lvmBugCleanup(path)

def _remove(path):
    cmd = [CMD_LVREMOVE, "-f", path]
    ret = util.pread2(cmd)

def rename(path, newName):
    cmd = [CMD_LVRENAME, path, newName]
    util.pread(cmd)

def setReadonly(path, readonly):
    val = "r"
    if not readonly:
        val += "w"
    cmd = [CMD_LVCHANGE, path, "-p", val]
    ret = util.pread(cmd)

#def getSize(path):
#    return _getLVsize(path)
#    #cmd = [CMD_LVS, "--noheadings", "--units", "B", path]
#    #ret = util.pread2(cmd)
#    #size = int(ret.strip().split()[-1][:-1])
#    #return size

def setSize(path, size, confirm):
    sizeMB = size / (1024 * 1024)
    cmd = [CMD_LVRESIZE, "-L", str(sizeMB), path]
    if confirm:
        util.pread3(cmd, "y\n")
    else:
        util.pread(cmd)

#def getTagged(path, tag):
#    """Return LV names of all LVs that have tag 'tag'; 'path' is either a VG
#    path or the entire LV path"""
#    tagged = []
#    cmd = [CMD_LVS, "--noheadings", "-o", "lv_name,lv_tags", path]
#    text = util.pread(cmd)
#    for line in text.split('\n'):
#        if not line:
#            continue
#        fields = line.split()
#        lvName = fields[0]
#        if len(fields) >= 2:
#            tags = fields[1]
#            if tags.find(tag) != -1:
#                tagged.append(lvName)
#    return tagged

#def getHidden(path):
#    return len(getTagged(path, LV_TAG_HIDDEN)) == 1

def setHidden(path, hidden = True):
    opt = "--addtag"
    if not hidden:
        opt = "--deltag"
    cmd = [CMD_LVCHANGE, opt, LV_TAG_HIDDEN, path]
    util.pread2(cmd)

def activateNoRefcount(path, refresh):
    cmd = [CMD_LVCHANGE, "-ay", path]
    text = util.pread2(cmd)
    if not _checkActive(path):
        raise util.CommandException(-1, str(cmd), "LV not activated")
    if refresh:
        # Override slave mode lvm.conf for this command
        os.environ['LVM_SYSTEM_DIR'] = MASTER_LVM_CONF
        cmd = [CMD_LVCHANGE, "--refresh", path]
        text = util.pread2(cmd)
        mapperDevice = path[5:].replace("-", "--").replace("/", "-")
        cmd = [CMD_DMSETUP, "table", mapperDevice]
        ret = util.pread(cmd)
        util.SMlog("DM table for %s: %s" % (path, ret.strip()))
        # Restore slave mode lvm.conf
        os.environ['LVM_SYSTEM_DIR'] = DEF_LVM_CONF

def deactivateNoRefcount(path):
    # LVM has a bug where if an "lvs" command happens to run at the same time 
    # as "lvchange -an", it might hold the device in use and cause "lvchange 
    # -an" to fail. Thus, we need to retry if "lvchange -an" fails. Worse yet, 
    # the race could lead to "lvchange -an" starting to deactivate (removing 
    # the symlink), failing to "dmsetup remove" the device, and still returning  
    # success. Thus, we need to check for the device mapper file existence if 
    # "lvchange -an" returns success. 
    for i in range(LVM_FAIL_RETRIES):
        try:
            _deactivate(path)
            break
        except util.CommandException:
            if i >= LVM_FAIL_RETRIES - 1:
                raise
            util.SMlog("*** lvchange -an failed on attempt #%d" % i)
    _lvmBugCleanup(path)

def _deactivate(path):
    cmd = [CMD_LVCHANGE, "-an", path]
    text = util.pread2(cmd)

#def getLVInfo(path):
#    cmd = [CMD_LVS, "--noheadings", "--units", "b", "-o", "+lv_tags", path]
#    text = util.pread2(cmd)
#    lvs = dict()
#    for line in text.split('\n'):
#        if not line:
#            continue
#        fields = line.split()
#        lvName = fields[0]
#        lvInfo = LVInfo(lvName)
#        lvInfo.size = long(fields[3].replace("B",""))
#        lvInfo.active = (fields[2][4] == 'a')
#        lvInfo.open = (fields[2][5] == 'o')
#        lvInfo.readonly = (fields[2][1] == 'r')
#        if len(fields) >= 5 and fields[4] == LV_TAG_HIDDEN:
#            lvInfo.hidden = True
#        lvs[lvName] = lvInfo
#    return lvs

def _checkActive(path):
    if util.pathexists(path):
        return True

    util.SMlog("_checkActive: %s does not exist!" % path)
    symlinkExists = os.path.lexists(path)
    util.SMlog("_checkActive: symlink exists: %s" % symlinkExists)

    mapperDeviceExists = False
    mapperDevice = path[5:].replace("-", "--").replace("/", "-")
    cmd = [CMD_DMSETUP, "status", mapperDevice]
    try:
        ret = util.pread2(cmd)
        mapperDeviceExists = True
        util.SMlog("_checkActive: %s: %s" % (mapperDevice, ret))
    except util.CommandException:
        util.SMlog("_checkActive: device %s does not exist" % mapperDevice)

    mapperPath = "/dev/mapper/" + mapperDevice
    mapperPathExists = util.pathexists(mapperPath)
    util.SMlog("_checkActive: path %s exists: %s" % \
            (mapperPath, mapperPathExists))

    if mapperDeviceExists and mapperPathExists and not symlinkExists:
        # we can fix this situation manually here
        try:
            util.SMlog("_checkActive: attempt to create the symlink manually.")
            os.symlink(mapperPath, path)
        except OSError, e:
            util.SMlog("ERROR: failed to symlink!")
            if e.errno != errno.EEXIST:
                raise
        if util.pathexists(path):
            util.SMlog("_checkActive: created the symlink manually")
            return True

    return False

def _lvmBugCleanup(path):
    # the device should not exist at this point. If it does, this was an LVM 
    # bug, and we manually clean up after LVM here
    mapperDevice = path[5:].replace("-", "--").replace("/", "-")
    mapperPath = "/dev/mapper/" + mapperDevice
            
    nodeExists = False
    cmd = [CMD_DMSETUP, "status", mapperDevice]
    try:
        util.pread(cmd, expect_rc=1)
    except util.CommandException, e:
        if e.code == 0:
            nodeExists = True

    if not util.pathexists(mapperPath) and not nodeExists:
        return

    util.SMlog("_lvmBugCleanup: seeing dm file %s" % mapperPath)

    # destroy the dm device
    if nodeExists:
        util.SMlog("_lvmBugCleanup: removing dm device %s" % mapperDevice)
        cmd = [CMD_DMSETUP, "remove", mapperDevice]
        for i in range(LVM_FAIL_RETRIES):
            try:
                util.pread2(cmd)
                break
            except util.CommandException, e:
                if i < LVM_FAIL_RETRIES - 1:
                    util.SMlog("Failed on try %d, retrying" % i)
                    time.sleep(1)
                else:
                    # make sure the symlink is still there for consistency
                    if not os.path.lexists(path):
                        os.symlink(mapperPath, path)
                        util.SMlog("_lvmBugCleanup: restored symlink %s" % path)
                    raise e

    if util.pathexists(mapperPath):
        os.unlink(mapperPath)
        util.SMlog("_lvmBugCleanup: deleted devmapper file %s" % mapperPath)

    # delete the symlink
    if os.path.lexists(path):
        os.unlink(path)
        util.SMlog("_lvmBugCleanup: deleted symlink %s" % path)

# mdpath is of format /dev/VG-SR-UUID/MGT
# or in other words /VG_LOCATION/VG_PREFIXSR-UUID/MDVOLUME_NAME
def ensurePathExists(mdpath):
    if not os.path.exists(mdpath):
        vgname = mdpath.split('/')[2]
        lvmCache = lvmcache.LVMCache(vgname)
        lvmCache.activateNoRefcount(MDVOLUME_NAME)
        
def removeDevMapperEntry(path):
    try:    
        # remove devmapper entry using dmsetup
        cmd = [CMD_DMSETUP, "remove", path]
        util.pread2(cmd)
        return True
    except Exception, e:
        util.SMlog("removeDevMapperEntry: dmsetup remove failed for file %s " \
                   "with error %s." % (path, str(e)))
        return False
    
