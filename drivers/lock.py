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

"""Serialization for concurrent operations"""

import os, stat, errno
import time
import flock
import util

VERBOSE = True

class LockException(util.SMException):
    pass

class Lock:
    """Simple file-based lock on a local FS. With shared reader/writer
    attributes."""

    BASE_DIR = "/var/lock/sm"

    def _open(self):
        """Create and open the lockable attribute base, if it doesn't exist.
        (But don't lock it yet.)"""

        # one directory per namespace
        self.nspath = os.path.join(Lock.BASE_DIR, self.ns)
        self._mkdirs(self.nspath)

        # the lockfile inside that namespace directory per namespace
        self.lockpath = os.path.join(self.nspath, self.name)
        if not os.path.exists(self.lockpath):
            util.SMlog("lock: creating lock file %s" % self.lockpath)
        self.lockfile = file(self.lockpath, "w+")

        fd = self.lockfile.fileno()
        self.lock = flock.WriteLock(fd)

    def _close(self):
        """Close the lock, which implies releasing the lock."""
        if self.lockfile is not None:
            if self.held():
                self.release()
            self.lockfile.close()
            util.SMlog("lock: closed %s" % self.lockpath)
            self.lockfile = None

    def _mknamespace(ns):

        if ns is None:
            return ".nil"

        assert not ns.startswith(".")
        assert ns.find(os.path.sep) < 0
        return ns
    _mknamespace = staticmethod(_mknamespace)

    def __init__(self, name, ns=None):
        self.lockfile = None

        self.ns = Lock._mknamespace(ns)

        assert not name.startswith(".")
        assert name.find(os.path.sep) < 0
        self.name = name

        self._open()

    __del__ = _close

    def cleanup(name, ns = None):
        ns = Lock._mknamespace(ns)
        path = os.path.join(Lock.BASE_DIR, ns, name)
        if os.path.exists(path):
            Lock._unlink(path)

    cleanup = staticmethod(cleanup)

    def cleanupAll(ns = None):
        ns = Lock._mknamespace(ns)
        nspath = os.path.join(Lock.BASE_DIR, ns)

        if not os.path.exists(nspath):
            return

        for file in os.listdir(nspath):
            path = os.path.join(nspath, file)
            Lock._unlink(path)

        Lock._rmdir(nspath)

    cleanupAll = staticmethod(cleanupAll)

    #
    # Lock and attribute file management
    #

    def _mkdirs(path):
        """Concurrent makedirs() catching EEXIST."""
        if os.path.exists(path):
            return
        try:
            os.makedirs(path)
        except OSError, e:
            if e.errno != errno.EEXIST:
                raise LockException("Failed to makedirs(%s)" % path)
    _mkdirs = staticmethod(_mkdirs)

    def _unlink(path):
        """Non-raising unlink()."""
        util.SMlog("lock: unlinking lock file %s" % path)
        try:
            os.unlink(path)
        except Exception, e:
            util.SMlog("Failed to unlink(%s): %s" % (path, e))
    _unlink = staticmethod(_unlink)

    def _rmdir(path):
        """Non-raising rmdir()."""
        util.SMlog("lock: removing lock dir %s" % path)
        try:
            os.rmdir(path)
        except Exception, e:
            util.SMlog("Failed to rmdir(%s): %s" % (path, e))
    _rmdir = staticmethod(_rmdir)

    #
    # Actual Locking
    #

    def acquire(self):
        """Blocking lock aquisition, with warnings. We don't expect to lock a
        lot. If so, not to collide. Coarse log statements should be ok
        and aid debugging."""
        if not self.lock.trylock():
            util.SMlog("Failed to lock %s on first attempt, " % self.lockpath
                   + "blocked by PID %d" % self.lock.test())
            self.lock.lock()
        if VERBOSE:
            util.SMlog("lock: acquired %s" % self.lockpath)

    def acquireNoblock(self):
        """Acquire lock if possible, or return false if lock already held"""
        exists = os.path.exists(self.lockpath)
        ret = self.lock.trylock()
        if VERBOSE:
            util.SMlog("lock: tried lock %s, acquired: %s (exists: %s)" % \
                    (self.lockpath, ret, exists))
        return ret

    def held(self):
        """True if @self acquired the lock, False otherwise."""
        return self.lock.held()

    def release(self):
        """Release a previously acquired lock."""
        self.lock.unlock()
        if VERBOSE:
            util.SMlog("lock: released %s" % self.lockpath)

if __debug__:
    import sys

    def test():

        # Create a Lock
        lock = Lock("test");

        # Should not be yet held.
        assert lock.held() == False

        # Go get it
        lock.acquire()

        # Second lock shall throw in debug mode.
        try:
            lock.acquire()
        except AssertionError, e:
            if str(e) != flock.WriteLock.ERROR_ISLOCKED:
                raise
        else:
            raise AssertionError("Reaquired a locked lock")

        lock.release()

        Lock.cleanup()

    if __name__ == '__main__':
        print >>sys.stderr, "Running self tests..."
        test()
        print >>sys.stderr, "OK."
