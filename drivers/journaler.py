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
# LVM-based journaling

import util
import errno
from srmetadata import open_file, close, get_min_blk_size_wrapper, \
    file_read_wrapper, file_write_wrapper

LVM_MAX_NAME_LEN = 127

class JournalerException(util.SMException):
    pass

class Journaler:
    """Simple journaler that uses LVM namespace for persistent "storage".
    A journal is a id-value pair, and there can be only one journal for a
    given id."""

    LV_SIZE = 4 * 1024 * 1024 # minimum size
    LV_TAG = "journaler"
    SEPARATOR = "_"
    JRN_CLONE = "clone"
    JRN_LEAF = "leaf"

    def __init__(self, lvmCache):
        self.vgName = lvmCache.vgName
        self.lvmCache = lvmCache

    def create(self, type, id, val):
        """Create an entry of type "type" for "id" with the value "val".
        Error if such an entry already exists."""
        valExisting = self.get(type, id)
        writeData = False
        if valExisting:
            raise JournalerException("Journal already exists for '%s:%s': %s" \
                    % (type, id, valExisting))
        lvName = self._getNameLV(type, id, val)

        mapperDevice = self._getLVMapperName(lvName)
        if len(mapperDevice) > LVM_MAX_NAME_LEN:
            lvName = self._getNameLV(type, id)
            writeData = True
            mapperDevice = self._getLVMapperName(lvName)
            assert len(mapperDevice) <= LVM_MAX_NAME_LEN

        self.lvmCache.create(lvName, self.LV_SIZE, self.LV_TAG)

        if writeData:
            fullPath = self.lvmCache._getPath(lvName)
            fd =  open_file(fullPath, True)
            try:
                e = None
                try:
                    min_block_size = get_min_blk_size_wrapper(fd)
                    data = "%d %s" % (len(val), val)
                    file_write_wrapper(fd, 0, min_block_size, data, len(data))
                except Exception, e:
                    raise
                finally:
                    try:
                        close(fd)
                        self.lvmCache.deactivateNoRefcount(lvName)
                    except Exception, e2:
                        msg = 'failed to close/deactivate %s: %s' \
                                % (lvName, e2)
                        if not e:
                            util.SMlog(msg)
                            raise e2
                        else:
                            util.SMlog('WARNING: %s (error ignored)' % msg)

            except:
                util.logException("journaler.create")
                try:
                    self.lvmCache.remove(lvName)
                except Exception, e:
                    util.SMlog('WARNING: failed to clean up failed journal ' \
                            ' creation: %s (error ignored)' % e)
                raise JournalerException("Failed to write to journal %s" \
                    % lvName)

    def remove(self, type, id):
        """Remove the entry of type "type" for "id". Error if the entry doesn't
        exist."""
        val = self.get(type, id)
        if not val:
            raise JournalerException("No journal for '%s:%s'" % (type, id))
        lvName = self._getNameLV(type, id, val)

        mapperDevice = self._getLVMapperName(lvName)
        if len(mapperDevice) > LVM_MAX_NAME_LEN:
            lvName = self._getNameLV(type, id)
        self.lvmCache.remove(lvName)

    def get(self, type, id):
        """Get the value for the journal entry of type "type" for "id".
        Return None if no such entry exists"""
        entries = self._getAllEntries()
        if not entries.get(type):
            return None
        return entries[type].get(id)

    def getAll(self, type):
        """Get a mapping id->value for all entries of type "type"."""
        entries = self._getAllEntries()
        if not entries.get(type):
            return dict()
        return entries[type]

    def hasJournals(self, id):
        """Return True if there any journals for "id", False otherwise"""
        entries = self._getAllEntries()
        for type, ids in entries.iteritems():
            if ids.get(id):
                return True
        return False

    def _getNameLV(self, type, id, val = 1):
        return "%s%s%s%s%s" % (type, self.SEPARATOR, id, self.SEPARATOR, val)

    def _getAllEntries(self):
        lvList = self.lvmCache.getTagged(self.LV_TAG)
        entries = dict()
        for lvName in lvList:
            parts = lvName.split(self.SEPARATOR, 2)
            if len(parts) != 3:
                raise JournalerException("Bad LV name: %s" % lvName)
            type, id, val = parts
            # For clone and leaf journals, additional
            # data is written inside file
            # TODO: Remove dependency on journal type
            try:
                if type == self.JRN_CLONE or type == self.JRN_LEAF:
                    fullPath = self.lvmCache._getPath(lvName)
                    self.lvmCache.activateNoRefcount(lvName,False)
                    fd = open_file(fullPath)
                    try:
                        try:
                            min_block_size = get_min_blk_size_wrapper(fd)
                            data = file_read_wrapper(fd, 0, min_block_size, min_block_size)
                            length, val = data.split(" ", 1)
                            val = val[:int(length)]
                        except:
                            raise JournalerException("Failed to read from journal %s" \
                                % lvName)
                    finally:
                        close(fd)
                        self.lvmCache.deactivateNoRefcount(lvName)
            except OSError, e:
                if e.errno == errno.EIO:
                    util.SMlog("Ignoring EIO errors for journal %s" % lvName)
                    continue
                else:
                    raise
            except util.CommandException, e:
                if e.code == errno.ENOENT:
                    util.SMlog("Ignoring ENOENT errors for journal %s" % lvName)
                    continue
                else:
                    raise
            if not entries.get(type):
                entries[type] = dict()
            entries[type][id] = val
        return entries

    def _getLVMapperName(self, lvName):
        return '%s-%s' % (self.vgName.replace("-", "--"), lvName)

###########################################################################
#
#  Unit tests
#
import lvutil
import lvmcache

def _runTests(vgName):
    """Unit testing"""
    print "Running unit tests..."
    if not vgName:
        print "Error: missing VG name param"
        return 1
    if not lvutil._checkVG(vgName):
        print "Error: VG %s not found" % vgName
        return 1

    j = Journaler(lvmcache.LVMCache(vgName))
    if j.get("clone", "1"):
        print "get non-existing failed"
        return 1
    j.create("clone", "1", "a")
    val = j.get("clone", "1")
    if val != "a":
        print "create-get failed"
        return 1
    j.remove("clone", "1")
    if j.get("clone", "1"):
        print "remove failed"
        return 1
    j.create("modify", "X", "831_3")
    j.create("modify", "Z", "831_4")
    j.create("modify", "Y", "53_0")
    val = j.get("modify", "X")
    if val != "831_3":
        print "create underscore_val failed"
        return 1
    val = j.get("modify", "Y")
    if val != "53_0":
        print "create multiple id's failed"
        return 1
    entries = j.getAll("modify")
    if not entries.get("X") or not entries.get("Y") or \
            entries["X"] != "831_3"  or entries["Y"] != "53_0":
        print "getAll failed: %s" % entries
        return 1
    j.remove("modify", "X")
    val = j.getAll("modify")
    if val.get("X") or not val.get("Y") or val["Y"] != "53_0":
        print "remove(X) failed"
        return 1
    j.remove("modify", "Y")
    j.remove("modify", "Z")
    if j.get("modify", "Y"):
        print "remove(Y) failed"
        return 1
    if j.get("modify", "Z"):
        print "remove(Z) failed"
        return 1
    print "All tests passed"
    return 0

if __name__ == '__main__':
    import sys
    vgName = None
    if len(sys.argv) > 1:
        vgName = sys.argv[1]
    ret = _runTests(vgName)
    sys.exit(ret)
