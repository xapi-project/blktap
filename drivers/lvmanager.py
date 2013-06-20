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
# Manage LV activations
#

import time
import util
import lvhdutil

class LVManagerException(util.SMException):
    pass

class LVActivator:
    """Keep track of LV activations and make LV activations transactional,
    so that when the SM operation finishes, either all LVs that needed to be
    activated are activated (in the success case), or none of them are (in a
    failure case). We don't want leave a random subset of activated LVs if
    something failed part way through"""

    OPEN_RETRY_ATTEMPTS = 10
    OPEN_RETRY_INTERVAL = 2

    NORMAL = False
    BINARY = True

    TEMPORARY = False
    PERSISTENT = True


    def __init__(self, srUuid, lvmCache):
        self.ns = lvhdutil.NS_PREFIX_LVM + srUuid
        self.lvmCache = lvmCache
        self.lvActivations = dict()
        self.openFiles = dict()
        for persistent in [self.TEMPORARY, self.PERSISTENT]:
            self.lvActivations[persistent] = dict()
            for binary in [self.NORMAL, self.BINARY]:
                self.lvActivations[persistent][binary] = dict()

    def activate(self, uuid, lvName, binary, persistent = False):
        if self.lvActivations[persistent][binary].get(uuid):
            if persistent:
                raise LVManagerException("Double persistent activation: %s" % \
                        uuid)
            return

        self.lvActivations[persistent][binary][uuid] = lvName
        self.lvmCache.activate(self.ns, uuid, lvName, binary)

    def activateEnforce(self, uuid, lvName, lvPath):
        """incrementing the refcount is not enough to keep an LV activated if
        another party is unaware of refcounting. For example, blktap does 
        direct "lvchange -an, lvchange -ay" during VBD.attach/resume without
        any knowledge of refcounts. Therefore, we need to keep the device open 
        to prevent unwanted deactivations. Note that blktap can do "lvchange
        -an" the very moment we try to open the file, so retry on failure"""
        if self.lvActivations[self.TEMPORARY][self.NORMAL].get(uuid):
            return
        self.activate(uuid, lvName, self.NORMAL)

        f = None
        for i in range(self.OPEN_RETRY_ATTEMPTS):
            try:
                f = open(lvPath, 'r')
                break
            except IOError:
                util.SMlog("(Failed to open %s on attempt %d)" % (lvPath, i))
                time.sleep(self.OPEN_RETRY_INTERVAL)
        if not f:
            raise LVManagerException("Failed to open %s" % lvPath)
        self.openFiles[uuid] = f
        self.lvmCache.changeOpen(lvName, 1)

    def deactivateAll(self):
        # this is the cleanup step that will be performed even if the original 
        # operation failed - don't throw exceptions here
        success = True
        for persistent in [self.TEMPORARY, self.PERSISTENT]:
            for binary in [self.NORMAL, self.BINARY]:
                uuids = self.lvActivations[persistent][binary].keys()
                for uuid in uuids:
                    try:
                        self.deactivate(uuid, binary, persistent)
                    except:
                        success = False
                        util.logException("_deactivateAll")
        return success

    def deactivate(self, uuid, binary, persistent = False):
        lvName = self.lvActivations[persistent][binary][uuid]
        if self.openFiles.get(uuid):
            self.openFiles[uuid].close()
            del self.openFiles[uuid]
            self.lvmCache.changeOpen(lvName, -1)
        self.lvmCache.deactivate(self.ns, uuid, lvName, binary)
        del self.lvActivations[persistent][binary][uuid]

    def persist(self):
        """Only commit LV chain activations when all LVs have been successfully
        activated. This ensures that if there is a failure part way through,
        the entire chain activation will be rolled back and we aren't left with
        random active LVs"""
        for binary in [self.NORMAL, self.BINARY]:
            self.lvActivations[self.PERSISTENT][binary].clear()

    def replace(self, oldUuid, uuid, lvName, binary):
        del self.lvActivations[self.TEMPORARY][binary][oldUuid]
        self.lvActivations[self.TEMPORARY][binary][uuid] = lvName
        if self.openFiles.get(oldUuid):
            # an open fd follows the file object through renames
            assert(not self.openFiles.get(uuid))
            self.openFiles[uuid] = self.openFiles[oldUuid]
            del self.openFiles[oldUuid]

    def add(self, uuid, lvName, binary):
        self.lvActivations[self.TEMPORARY][binary][uuid] = lvName

    def remove(self, uuid, binary):
        if (self.openFiles.get(uuid)):
            raise LVManagerException("Open file reference for %s" % uuid)
        del self.lvActivations[self.TEMPORARY][binary][uuid]

    def get(self, uuid, binary):
        return self.lvActivations[self.TEMPORARY][binary].get(uuid)


