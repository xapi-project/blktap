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
# LVM cache (for minimizing the number of lvs commands)
#

import os
import util
import lvutil
from lock import Lock
from refcounter import RefCounter

class LVInfo:
    def __init__(self, name):
        self.name = name
        self.size = 0
        self.active = False
        self.open = 0
        self.readonly = False
        self.tags = []

    def toString(self):
        return "%s, size=%d, active=%s, open=%s, ro=%s, tags=%s" % \
                (self.name, self.size, self.active, self.open, self.readonly, \
                self.tags)

def lazyInit(op):
    def wrapper(self, *args):
        if not self.initialized:
            util.SMlog("LVMCache: will initialize now")
            self.refresh()
            #util.SMlog("%s(%s): %s" % (op, args, self.toString()))
        try:
            ret = op(self, *args)
        except KeyError:
            util.logException("LVMCache")
            util.SMlog("%s(%s): %s" % (op, args, self.toString()))
            raise
        return ret
    return wrapper


class LVMCache:
    """Per-VG object to store LV information. Can be queried for cached LVM
    information and refreshed"""

    def __init__(self, vgName):
        """Create a cache for VG vgName, but don't scan the VG yet"""
        self.vgName = vgName
        self.vgPath = "/dev/%s" % self.vgName
        self.lvs = dict()
        self.tags = dict()
        self.initialized = False
        util.SMlog("LVMCache created for %s" % vgName)

    def refresh(self):
        """Get the LV information for the VG using "lvs" """
        util.SMlog("LVMCache: refreshing")
        cmd = [lvutil.CMD_LVS,
                "--noheadings", "--units", "b", "-o", "+lv_tags", self.vgPath]
        text = util.pread2(cmd)
        self.lvs.clear()
        self.tags.clear()
        for line in text.split('\n'):
            if not line:
                continue
            fields = line.split()
            lvName = fields[0]
            lvInfo = LVInfo(lvName)
            lvInfo.size = long(fields[3].replace("B",""))
            lvInfo.active = (fields[2][4] == 'a')
            if (fields[2][5] == 'o'):
                lvInfo.open = 1
            lvInfo.readonly = (fields[2][1] == 'r')
            self.lvs[lvName] = lvInfo
            if len(fields) >= 5:
                tags = fields[4].split(',')
                for tag in tags:
                    self._addTag(lvName, tag)
        self.initialized = True

    #
    # lvutil functions
    #
    @lazyInit
    def create(self, lvName, size, tag = None, activate = True):
        lvutil.create(lvName, size, self.vgName, tag, activate)
        lvInfo = LVInfo(lvName)
        lvInfo.size = size
        lvInfo.active = activate
        self.lvs[lvName] = lvInfo
        if tag:
            self._addTag(lvName, tag)

    @lazyInit
    def remove(self, lvName):
        path = self._getPath(lvName)
        lvutil.remove(path)
        for tag in self.lvs[lvName].tags:
            self._removeTag(lvName, tag)
        del self.lvs[lvName]

    @lazyInit
    def rename(self, lvName, newName):
        path = self._getPath(lvName)
        lvutil.rename(path, newName)
        lvInfo = self.lvs[lvName]
        del self.lvs[lvName]
        lvInfo.name = newName
        self.lvs[newName] = lvInfo

    @lazyInit
    def setSize(self, lvName, newSize):
        path = self._getPath(lvName)
        size = self.getSize(lvName)
        lvutil.setSize(path, newSize, (newSize < size))
        self.lvs[lvName].size = newSize

    @lazyInit
    def activate(self, ns, ref, lvName, binary):
        lock = Lock(ref, ns)
        lock.acquire()
        try:
            count = RefCounter.get(ref, binary, ns)
            if count == 1:
                try:
                    self.activateNoRefcount(lvName)
                except util.CommandException:
                    RefCounter.put(ref, binary, ns)
                    raise
        finally:
            lock.release()

    @lazyInit
    def deactivate(self, ns, ref, lvName, binary):
        lock = Lock(ref, ns)
        lock.acquire()
        try:
            count = RefCounter.put(ref, binary, ns)
            if count > 0:
                return
            refreshed = False
            while True:
                lvInfo = self.getLVInfo(lvName)
                if len(lvInfo) != 1:
                    raise util.SMException("LV info not found for %s" % ref)
                info = lvInfo[lvName]
                if info.open:
                    if refreshed:
                        # should never happen in normal conditions but in some 
                        # failure cases the recovery code may not be able to 
                        # determine what the correct refcount should be, so it 
                        # is not unthinkable that the value might be out of 
                        # sync
                        util.SMlog("WARNING: deactivate: LV %s open" % lvName)
                        return
                    # check again in case the cached value is stale
                    self.refresh()
                    refreshed = True
                else:
                    break
            try:
                self.deactivateNoRefcount(lvName)
            except util.CommandException:
                self.refresh()
                if self.getLVInfo(lvName):
                    util.SMlog("LV %s could not be deactivated" % lvName)
                    if lvInfo[lvName].active:
                        util.SMlog("Reverting the refcount change")
                        RefCounter.get(ref, binary, ns)
                    raise
                else:
                    util.SMlog("LV %s not found" % lvName)
        finally:
            lock.release()

    @lazyInit
    def activateNoRefcount(self, lvName, refresh = False):
        path = self._getPath(lvName)
        lvutil.activateNoRefcount(path, refresh)
        self.lvs[lvName].active = True

    @lazyInit
    def deactivateNoRefcount(self, lvName):
        path = self._getPath(lvName)
        if self.checkLV(lvName):
            lvutil.deactivateNoRefcount(path)
            self.lvs[lvName].active = False
        else:
            util.SMlog("LVMCache.deactivateNoRefcount: no LV %s" % lvName)
            lvutil._lvmBugCleanup(path)

    @lazyInit
    def setHidden(self, lvName, hidden=True):
        path = self._getPath(lvName)
        if hidden:
            lvutil.setHidden(path)
            self._addTag(lvName, lvutil.LV_TAG_HIDDEN)
        else:
            lvutil.setHidden(path, hidden=False)
            self._removeTag(lvName, lvutil.LV_TAG_HIDDEN)

    @lazyInit
    def setReadonly(self, lvName, readonly):
        path = self._getPath(lvName)
        if self.lvs[lvName].readonly != readonly: 
            lvutil.setReadonly(path, readonly)
            self.lvs[lvName].readonly = readonly

    @lazyInit
    def changeOpen(self, lvName, inc):
        """We don't actually open or close the LV, just mark it in the cache"""
        self.lvs[lvName].open += inc


    #
    # cached access
    #
    @lazyInit
    def checkLV(self, lvName):
        return self.lvs.get(lvName)

    @lazyInit
    def getLVInfo(self, lvName = None):
        result = dict()
        lvs = []
        if lvName == None:
            lvs = self.lvs.keys()
        elif self.lvs.get(lvName):
            lvs = [lvName]
        for lvName in lvs:
            lvInfo = self.lvs[lvName]
            lvutilInfo = lvutil.LVInfo(lvName)
            lvutilInfo.size = lvInfo.size
            lvutilInfo.active = lvInfo.active
            lvutilInfo.open = (lvInfo.open > 0)
            lvutilInfo.readonly = lvInfo.readonly
            if lvutil.LV_TAG_HIDDEN in lvInfo.tags:
                lvutilInfo.hidden = True
            result[lvName] = lvutilInfo
        return result

    @lazyInit
    def getSize(self, lvName):
        return self.lvs[lvName].size

    @lazyInit
    def getHidden(self, lvName):
        return (lvutil.LV_TAG_HIDDEN in self.lvs[lvName].tags)

    @lazyInit
    def getTagged(self, tag):
        lvList = self.tags.get(tag)
        if not lvList:
            return []
        return lvList

    #
    # private
    #
    def _getPath(self, lvName):
        return os.path.join(self.vgPath, lvName)

    def _addTag(self, lvName, tag):
        self.lvs[lvName].tags.append(tag)
        if self.tags.get(tag):
            self.tags[tag].append(lvName)
        else:
            self.tags[tag] = [lvName]

    def _removeTag(self, lvName, tag):
        self.lvs[lvName].tags.remove(tag)
        self.tags[tag].remove(lvName)

    def toString(self):
        result = "LVM Cache for %s: %d LVs" % (self.vgName, len(self.lvs))
        for lvName, lvInfo in self.lvs.iteritems():
            result += "\n%s" % lvInfo.toString()
        return result
