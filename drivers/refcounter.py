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
# Synchronization must be done at a higher level, by the users of this module
#


import os
import util
from lock import Lock
import errno

class RefCounterException(util.SMException):
    pass

class RefCounter:
    """Persistent local-FS file-based reference counter. The
    operations are get() and put(), and they are atomic."""

    BASE_DIR = "/var/run/sm/refcount"

    def get(obj, binary, ns = None):
        """Get (inc ref count) 'obj' in namespace 'ns' (optional). 
        Returns new ref count"""
        if binary:
            return RefCounter._adjust(ns, obj, 0, 1)
        else:
            return RefCounter._adjust(ns, obj, 1, 0)
    get = staticmethod(get)

    def put(obj, binary, ns = None):
        """Put (dec ref count) 'obj' in namespace 'ns' (optional). If ref
        count was zero already, this operation is a no-op.
        Returns new ref count"""
        if binary:
            return RefCounter._adjust(ns, obj, 0, -1)
        else:
            return RefCounter._adjust(ns, obj, -1, 0)
    put = staticmethod(put)

    def set(obj, count, binaryCount, ns = None):
        """Set normal & binary counts explicitly to the specified values.
        Returns new ref count"""
        (obj, ns) = RefCounter._getSafeNames(obj, ns)
        assert(count >= 0 and binaryCount >= 0)
        if binaryCount > 1:
            raise RefCounterException("Binary count = %d > 1" % binaryCount)
        RefCounter._set(ns, obj, count, binaryCount)
    set = staticmethod(set)

    def check(obj, ns = None):
        """Get the ref count values for 'obj' in namespace 'ns' (optional)"""
        (obj, ns) = RefCounter._getSafeNames(obj, ns)
        return RefCounter._get(ns, obj)
    check = staticmethod(check)

    def checkLocked(obj, ns):
        """Lock-protected access"""
        lock = Lock(obj, ns)
        lock.acquire()
        try:
            return RefCounter.check(obj, ns)
        finally:
            lock.release()
    checkLocked = staticmethod(checkLocked)

    def reset(obj, ns = None):
        """Reset ref counts for 'obj' in namespace 'ns' (optional) to 0."""
        RefCounter.resetAll(ns, obj)
    reset = staticmethod(reset)

    def resetAll(ns = None, obj = None):
        """Reset ref counts of 'obj' in namespace 'ns' to 0. If obj is not
        provided, reset all existing objects in 'ns' to 0. If neither obj nor 
        ns are supplied, do this for all namespaces"""
        if obj:
            (obj, ns) = RefCounter._getSafeNames(obj, ns)
        if ns:
            nsList = [ns]
        else:
            if not util.pathexists(RefCounter.BASE_DIR):
                return
            try:
                nsList = os.listdir(RefCounter.BASE_DIR)
            except OSError:
                raise RefCounterException("failed to get namespace list")
        for ns in nsList:
            RefCounter._reset(ns, obj)
    resetAll = staticmethod(resetAll)

    def _adjust(ns, obj, delta, binaryDelta):
        """Add 'delta' to the normal refcount and 'binaryDelta' to the binary
        refcount of 'obj' in namespace 'ns'. 
        Returns new ref count"""
        if binaryDelta > 1 or binaryDelta < -1:
            raise RefCounterException("Binary delta = %d outside [-1;1]" % \
                    binaryDelta)
        (obj, ns) = RefCounter._getSafeNames(obj, ns)
        (count, binaryCount) = RefCounter._get(ns, obj)

        newCount = count + delta
        newBinaryCount = binaryCount + binaryDelta
        if newCount < 0:
            util.SMlog("WARNING: decrementing normal refcount of 0")
            newCount = 0
        if newBinaryCount < 0:
            util.SMlog("WARNING: decrementing binary refcount of 0")
            newBinaryCount = 0
        if newBinaryCount > 1:
            newBinaryCount = 1
        util.SMlog("Refcount for %s:%s (%d, %d) + (%d, %d) => (%d, %d)" % \
                (ns, obj, count, binaryCount, delta, binaryDelta,
                    newCount, newBinaryCount))
        RefCounter._set(ns, obj, newCount, newBinaryCount)
        return newCount + newBinaryCount
    _adjust = staticmethod(_adjust)

    def _get(ns, obj):
        """Get the ref count values for 'obj' in namespace 'ns'"""
        objFile = os.path.join(RefCounter.BASE_DIR, ns, obj)
        (count, binaryCount) = (0, 0)
        if util.pathexists(objFile):
            (count, binaryCount) = RefCounter._readCount(objFile)
        return (count, binaryCount)
    _get = staticmethod(_get)

    def _set(ns, obj, count, binaryCount):
        """Set the ref count values for 'obj' in namespace 'ns'"""
        util.SMlog("Refcount for %s:%s set => (%d, %db)" % \
                (ns, obj, count, binaryCount))
        if count == 0 and binaryCount == 0:
            RefCounter._removeObject(ns, obj)
        else:
            RefCounter._createNamespace(ns)
            objFile = os.path.join(RefCounter.BASE_DIR, ns, obj)
            RefCounter._writeCount(objFile, count, binaryCount)
    _set = staticmethod(_set)

    def _getSafeNames(obj, ns):
        """Get a name that can be used as a file name"""
        if not ns:
            ns = obj.split('/')[0]
            if not ns:
                ns = "default"
        for char in ['/', '*', '?', '\\']:
            obj = obj.replace(char, "_")
        return (obj, ns)
    _getSafeNames = staticmethod(_getSafeNames)

    def _createNamespace(ns):
        nsDir = os.path.join(RefCounter.BASE_DIR, ns)
        try:
            os.makedirs(nsDir)
        except OSError, e:
            if e.errno != errno.EEXIST:
                raise RefCounterException("failed to makedirs '%s' (%s)" % \
                        (nsDir, e))
    _createNamespace = staticmethod(_createNamespace)

    def _removeObject(ns, obj):
        nsDir = os.path.join(RefCounter.BASE_DIR, ns)
        objFile = os.path.join(nsDir, obj)
        if not util.pathexists(objFile):
            return
        try:
            os.unlink(objFile)
        except OSError:
            raise RefCounterException("failed to remove '%s'" % objFile)
        if not os.listdir(nsDir):
            try:
                os.rmdir(nsDir)
            except OSError:
                raise RefCounterException("failed to remove '%s'" % nsDir)
    _removeObject = staticmethod(_removeObject)

    def _reset(ns, obj = None):
        nsDir = os.path.join(RefCounter.BASE_DIR, ns)
        if not util.pathexists(nsDir):
            return
        if obj:
            if not util.pathexists(os.path.join(nsDir, obj)):
                return
            objList = [obj]
        else:
            try:
                objList = os.listdir(nsDir)
            except OSError:
                raise RefCounterException("failed to list '%s'" % ns)
        for obj in objList:
            RefCounter._removeObject(ns, obj)
    _reset = staticmethod(_reset)

    def _readCount(fn):
        try:
            f = open(fn, 'r')
            line = f.readline()
            nums = line.split()
            count = int(nums[0])
            binaryCount = int(nums[1])
            f.close()
        except IOError:
            raise RefCounterException("failed to read file '%s'" % fn)
        return (count, binaryCount)
    _readCount = staticmethod(_readCount)

    def _writeCount(fn, count, binaryCount):
        try:
            f = open(fn, 'w')
            f.write("%d %d\n" % (count, binaryCount))
            f.close()
        except IOError:
            raise RefCounterException("failed to write '(%d %d)' to '%s'" % \
                    (count, binaryCount, fn))
    _writeCount = staticmethod(_writeCount)


    def _runTests():
        "Unit tests"

        RefCounter.resetAll()

        # A
        (cnt, bcnt) = RefCounter.check("X", "A")
        if cnt != 0 or bcnt != 0:
            print "Error: check = %d != 0 in the beginning" % cnt
            return -1

        cnt = RefCounter.get("X", False, "A")
        if cnt != 1:
            print "Error: count = %d != 1 after first get()" % cnt
            return -1
        (cnt, bcnt) = RefCounter.check("X", "A")
        if cnt != 1:
            print "Error: check = %d != 1 after first get()" % cnt
            return -1

        cnt = RefCounter.put("X", False, "A")
        if cnt != 0:
            print "Error: count = %d != 0 after get-put" % cnt
            return -1
        (cnt, bcnt) = RefCounter.check("X", "A")
        if cnt != 0:
            print "Error: check = %d != 0 after get-put" % cnt
            return -1

        cnt = RefCounter.get("X", False, "A")
        if cnt != 1:
            print "Error: count = %d != 1 after get-put-get" % cnt
            return -1

        cnt = RefCounter.get("X", False, "A")
        if cnt != 2:
            print "Error: count = %d != 2 after second get()" % cnt
            return -1

        cnt = RefCounter.get("X", False, "A")
        if cnt != 3:
            print "Error: count = %d != 3 after third get()" % cnt
            return -1
        (cnt, bcnt) = RefCounter.check("X", "A")
        if cnt != 3:
            print "Error: check = %d != 3 after third get()" % cnt
            return -1

        cnt = RefCounter.put("Y", False, "A")
        if cnt != 0:
            print "Error: count = %d != 0 after first put()" % cnt
            return -1
        (cnt, bcnt) = RefCounter.check("Y", "A")
        if cnt != 0:
            print "Error: check = %d != 0 after first put()" % cnt
            return -1

        cnt = RefCounter.put("X", False, "A")
        if cnt != 2:
            print "Error: count = %d != 2 after 3get-1put" % cnt
            return -1

        cnt = RefCounter.put("X", False, "A")
        if cnt != 1:
            print "Error: count = %d != 1 after 3get-2put" % cnt
            return -1

        cnt = RefCounter.get("X", False, "A")
        if cnt != 2:
            print "Error: count = %d != 2 after 4get-2put" % cnt
            return -1
        (cnt, bcnt) = RefCounter.check("X", "A")
        if cnt != 2:
            print "Error: check = %d != 2 after 4get-2put" % cnt
            return -1

        cnt = RefCounter.put("X", False, "A")
        if cnt != 1:
            print "Error: count = %d != 0 after 4get-3put" % cnt
            return -1

        cnt = RefCounter.put("X", False, "A")
        if cnt != 0:
            print "Error: count = %d != 0 after 4get-4put" % cnt
            return -1
        (cnt, bcnt) = RefCounter.check("X", "A")
        if cnt != 0:
            print "Error: check = %d != 0 after 4get-4put" % cnt
            return -1

        # B
        cnt = RefCounter.put("Z", False, "B")
        if cnt != 0:
            print "Error: count = %d != 0 after new put()" % cnt
            return -1

        cnt = RefCounter.get("Z", False, "B")
        if cnt != 1:
            print "Error: count = %d != 1 after put-get" % cnt
            return -1

        cnt = RefCounter.put("Z", False, "B")
        if cnt != 0:
            print "Error: count = %d != 0 after put-get-put" % cnt
            return -1
        (cnt, bcnt) = RefCounter.check("Z", "B")
        if cnt != 0:
            print "Error: check = %d != 0 after put-get-put" % cnt
            return -1

        cnt = RefCounter.get("Z", False, "B")
        if cnt != 1:
            print "Error: count = %d != 1 after put-get-put-get" % cnt
            return -1
        (cnt, bcnt) = RefCounter.check("Z", "B")
        if cnt != 1:
            print "Error: check = %d != 1 after put-get-put-get" % cnt
            return -1

        # set
        (cnt, bcnt) = RefCounter.check("a/b")
        if cnt != 0:
            print "Error: count = %d != 0 initially" % cnt
            return -1
        RefCounter.set("a/b", 2, 0)
        (cnt, bcnt) = RefCounter.check("a/b")
        if cnt != 2 or bcnt != 0:
            print "Error: count = (%d,%d) != (2,0) after set(2,0)" % (cnt, bcnt)
            return -1
        cnt = RefCounter.put("a/b", False)
        if cnt != 1:
            print "Error: count = %d != 1 after set(2)-put" % cnt
            return -1
        cnt = RefCounter.get("a/b", False)
        if cnt != 2:
            print "Error: count = %d != 2 after set(2)-put-get" % cnt
            return -1
        RefCounter.set("a/b", 100, 0)
        (cnt, bcnt) = RefCounter.check("a/b")
        if cnt != 100 or bcnt != 0:
            print "Error: cnt,bcnt = (%d,%d) != (100,0) after set(100,0)" % \
                    (cnt, bcnt)
            return -1
        cnt = RefCounter.get("a/b", False)
        if cnt != 101:
            print "Error: count = %d != 101 after get" % cnt
            return -1
        RefCounter.set("a/b", 100, 1)
        (cnt, bcnt) = RefCounter.check("a/b")
        if cnt != 100 or bcnt != 1:
            print "Error: cnt,bcnt = (%d,%d) != (100,1) after set(100,1)" % \
                    (cnt, bcnt)
            return -1
        RefCounter.reset("a/b")
        (cnt, bcnt) = RefCounter.check("a/b")
        if cnt != 0:
            print "Error: check = %d != 0 after reset" % cnt
            return -1

        # binary
        cnt = RefCounter.get("A", True)
        if cnt != 1:
            print "Error: count = %d != 1 after get(bin)" % cnt
            return -1
        cnt = RefCounter.get("A", True)
        if cnt != 1:
            print "Error: count = %d != 1 after get(bin)*2" % cnt
            return -1
        cnt = RefCounter.put("A", True)
        if cnt != 0:
            print "Error: count = %d != 0 after get(bin)*2-put(bin)" % cnt
            return -1
        cnt = RefCounter.put("A", True)
        if cnt != 0:
            print "Error: count = %d != 0 after get(bin)*2-put(bin)*2" % cnt
            return -1
        try:
            RefCounter.set("A", 0, 2)
            print "Error: set(0,2) was allowed"
            return -1
        except RefCounterException:
            pass
        cnt = RefCounter.get("A", True)
        if cnt != 1:
            print "Error: count = %d != 1 after get(bin)" % cnt
            return -1
        cnt = RefCounter.get("A", False)
        if cnt != 2:
            print "Error: count = %d != 2 after get(bin)-get" % cnt
            return -1
        cnt = RefCounter.get("A", False)
        if cnt != 3:
            print "Error: count = %d != 3 after get(bin)-get-get" % cnt
            return -1
        cnt = RefCounter.get("A", True)
        if cnt != 3:
            print "Error: count = %d != 3 after get(bin)-get*2-get(bin)" % cnt
            return -1
        cnt = RefCounter.put("A", False)
        if cnt != 2:
            print "Error: count = %d != 2 after get(bin)*2-get*2-put" % cnt
            return -1
        cnt = RefCounter.put("A", True)
        if cnt != 1:
            print "Error: cnt = %d != 1 after get(b)*2-get*2-put-put(b)" % cnt
            return -1
        cnt = RefCounter.put("A", False)
        if cnt != 0:
            print "Error: cnt = %d != 0 after get(b)*2-get*2-put*2-put(b)" % cnt
            return -1

        # names
        cnt = RefCounter.get("Z", False)
        if cnt != 1:
            print "Error: count = %d != 1 after get (no ns 1)" % cnt
            return -1

        cnt = RefCounter.get("Z/", False)
        if cnt != 1:
            print "Error: count = %d != 1 after get (no ns 2)" % cnt
            return -1

        cnt = RefCounter.get("/Z", False)
        if cnt != 1:
            print "Error: count = %d != 1 after get (no ns 3)" % cnt
            return -1

        cnt = RefCounter.get("/Z/*/?/\\", False)
        if cnt != 1:
            print "Error: count = %d != 1 after get (no ns 4)" % cnt
            return -1

        cnt = RefCounter.get("Z", False)
        if cnt != 2:
            print "Error: count = %d != 2 after get (no ns 1)" % cnt
            return -1

        cnt = RefCounter.get("Z/", False)
        if cnt != 2:
            print "Error: count = %d != 2 after get (no ns 2)" % cnt
            return -1

        cnt = RefCounter.get("/Z", False)
        if cnt != 2:
            print "Error: count = %d != 2 after get (no ns 3)" % cnt
            return -1

        cnt = RefCounter.get("/Z/*/?/\\", False)
        if cnt != 2:
            print "Error: count = %d != 2 after get (no ns 4)" % cnt
            return -1

        # resetAll
        RefCounter.resetAll("B")
        cnt = RefCounter.get("Z", False, "B")
        if cnt != 1:
            print "Error: count = %d != 1 after resetAll-get" % cnt
            return -1

        cnt = RefCounter.get("Z", False, "C")
        if cnt != 1:
            print "Error: count = %d != 1 after C.get" % cnt
            return -1

        RefCounter.resetAll("B")
        cnt = RefCounter.get("Z", False, "B")
        if cnt != 1:
            print "Error: count = %d != 1 after second resetAll-get" % cnt
            return -1

        cnt = RefCounter.get("Z", False, "C")
        if cnt != 2:
            print "Error: count = %d != 2 after second C.get" % cnt
            return -1

        RefCounter.resetAll("D")
        RefCounter.resetAll()
        cnt = RefCounter.put("Z", False, "B")
        if cnt != 0:
            print "Error: count = %d != 0 after resetAll-put" % cnt
            return -1

        cnt = RefCounter.put("Z", False, "C")
        if cnt != 0:
            print "Error: count = %d != 0 after C.resetAll-put" % cnt
            return -1

        RefCounter.resetAll()

        return 0
    _runTests = staticmethod(_runTests)


if __name__ == '__main__':
    print "Running unit tests..."
    try:
        if RefCounter._runTests() == 0:
            print "All done, no errors"
    except RefCounterException, e:
        print "FAIL: Got exception: %s" % e
        raise
