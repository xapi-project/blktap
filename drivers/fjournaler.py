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
# File-based journaling

import os
import errno

import util
from journaler import JournalerException

SEPARATOR = "_"

class Journaler:
    """Simple file-based journaler. A journal is a id-value pair, and there
    can be only one journal for a given id."""

    def __init__(self, dir):
        self.dir = dir

    def create(self, type, id, val):
        """Create an entry of type "type" for "id" with the value "val".
        Error if such an entry already exists."""
        valExisting = self.get(type, id)
        if valExisting:
            raise JournalerException("Journal already exists for '%s:%s': %s" \
                    % (type, id, valExisting))
        path = self._getPath(type, id)
        f = open(path, "w")
        f.write(val)
        f.close()

    def remove(self, type, id):
        """Remove the entry of type "type" for "id". Error if the entry doesn't
        exist."""
        val = self.get(type, id)
        if not val:
            raise JournalerException("No journal for '%s:%s'" % (type, id))
        path = self._getPath(type, id)
        os.unlink(path)

    def get(self, type, id):
        """Get the value for the journal entry of type "type" for "id".
        Return None if no such entry exists"""
        path = self._getPath(type, id)
        if not util.pathexists(path):
            return None
        try:
            f = open(path, "r")
        except IOError, e:
            if e.errno == errno.ENOENT:
                # the file can disappear any time, since there is no locking
                return None 
            raise
        val = f.readline()
        return val

    def getAll(self, type):
        """Get a mapping id->value for all entries of type "type" """
        fileList = os.listdir(self.dir)
        entries = dict()
        for fileName in fileList:
            if not fileName.startswith(type):
                continue
            parts = fileName.split(SEPARATOR, 2)
            if len(parts) != 2:
                raise JournalerException("Bad file name: %s" % fileName)
            t, id = parts
            if t != type:
                continue
            val = self.get(type, id)
            if val:
                entries[id] = val
        return entries

    def _getPath(self, type, id):
        name = "%s%s%s" % (type, SEPARATOR, id)
        path = os.path.join(self.dir, name)
        return path


###########################################################################
#
#  Unit tests
#
def _runTests():
    """Unit testing"""
    dir = "/tmp"
    print "Running unit tests..."

    j = Journaler(dir)
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
    ret = _runTests()
    sys.exit(ret)
