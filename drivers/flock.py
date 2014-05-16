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

"""
Fcntl-based Advisory Locking with a proper .trylock()

Python's fcntl module is not good at locking. In particular, proper
testing and trying of locks isn't well supported. Looks as if we've
got to grow our own.
"""

import os, fcntl, struct
import errno

class Flock:
    """A C flock struct."""

    def __init__(self, l_type, l_whence=0, l_start=0, l_len=0, l_pid=0):
        """See fcntl(2) for field details."""
        self.fields = [l_type, l_whence, l_start, l_len, l_pid]

    FORMAT = "hhqql"
    # struct flock(2) format, tested with python2.4/i686 and
    # python2.5/x86_64. http://docs.python.org/lib/posix-large-files.html
        
    def fcntl(self, fd, cmd):
        """Issues a system fcntl(fd, cmd, self). Updates self with what was
        returned by the kernel. Otherwise raises IOError(errno)."""

        st = struct.pack(self.FORMAT, *self.fields)
        st = fcntl.fcntl(fd, cmd, st)

        fields = struct.unpack(self.FORMAT, st)
        self.__init__(*fields)

    FIELDS = { 'l_type':       0,
               'l_whence':     1,
               'l_start':      2,
               'l_len':        3,
               'l_pid':        4 }

    def __getattr__(self, name):
        idx = self.FIELDS[name]
        return self.fields[idx]

    def __setattr__(self, name, value):
        idx = self.FIELDS.get(name)
        if idx is None:
            self.__dict__[name] = value
        else:
            self.fields[idx] = value

class FcntlLockBase:
    """Abstract base class for either reader or writer locks. A respective
    definition of LOCK_TYPE (fcntl.{F_RDLCK|F_WRLCK}) determines the
    type."""
    
    if __debug__:
        ERROR_ISLOCKED = "Attempt to acquire lock held."
        ERROR_NOTLOCKED = "Attempt to unlock lock not held."

    def __init__(self, fd):
        """Creates a new, unheld lock."""
        self.fd = fd
        #
        # Subtle: fcntl(2) permits re-locking it as often as you want
        # once you hold it. This is slightly counterintuitive and we
        # want clean code, so we add one bit of our own bookkeeping.
        #
        self._held = False

    def lock(self):
        """Blocking lock aquisition."""
        assert not self._held, self.ERROR_ISLOCKED
        Flock(self.LOCK_TYPE).fcntl(self.fd, fcntl.F_SETLKW)
        self._held = True

    def trylock(self):
        """Non-blocking lock aquisition. Returns True on success, False
        otherwise."""
        if self._held: return False
        try:
            Flock(self.LOCK_TYPE).fcntl(self.fd, fcntl.F_SETLK)
        except IOError, e:
            if e.errno in [errno.EACCES, errno.EAGAIN]:
                return False
            raise
        self._held = True
        return True

    def held(self):
        """Returns True if @self holds the lock, False otherwise."""
        return self._held

    def unlock(self):
        """Release a previously acquired lock."""
        Flock(fcntl.F_UNLCK).fcntl(self.fd, fcntl.F_SETLK)
        self._held = False

    def test(self):
        """Returns the PID of the process holding the lock or -1 if the lock
        is not held."""
        if self._held: return os.getpid()
        flock = Flock(self.LOCK_TYPE)
        flock.fcntl(self.fd, fcntl.F_GETLK)
        if flock.l_type == fcntl.F_UNLCK:
            return -1
        return flock.l_pid


class WriteLock(FcntlLockBase):
    """A simple global writer (i.e. exclusive) lock."""
    LOCK_TYPE = fcntl.F_WRLCK

class ReadLock(FcntlLockBase):
    """A simple global reader (i.e. shared) lock."""
    LOCK_TYPE = fcntl.F_RDLCK


#
# Test/Example
#
if __debug__:
    import sys

    def test_interface():

        lockfile = file("/tmp/lockfile", "w+")
    
        # Create a WriteLock
        fd = lockfile.fileno()
        lock = WriteLock(fd)

        # It's not yet held.
        assert lock.test() == None
        assert lock.held() == False
         
        #
        # Let a child aquire it
        #
    
        (pin, cout) = os.pipe()
        (cin, pout) = os.pipe()

        pid = os.fork()
        if pid == 0:
            os.close(pin)
            os.close(pout)
    
            lock.lock()
    
            # Synchronize
            os.write(cout, "SYN")
    
            # Wait for parent
            assert os.read(cin, 3) == "ACK", "Faulty parent"
            
            sys.exit(0)
    
        os.close(cout)
        os.close(cin)
    
        # Wait for child
        assert os.read(pin, 3) == "SYN", "Faulty child"
    
        # Lock should be held by child
        assert lock.test() == pid
        assert lock.trylock() == False
        assert lock.held() == False
    
        # Synchronize child
        os.write(pout, "ACK")
    
        # Lock requires our uncooperative child to terminate.
        lock.lock()
    
        # We got the lock, so child should have exited, right?
        #assert os.waitpid(pid, os.WNOHANG) == (pid, 0)
        #
        # Won't work but race, because the runtime will explicitly
        # lockfile.close() before the real exit(2). See
        # FcntlLockBase.__del__() above.
    
        # Attempt to re-lock should throw
        try:
            lock.lock()
        except AssertionError, e:
            if str(e) != WriteLock.ERROR_ISLOCKED:
                raise
        else:
            raise AssertionError("Held locks should not be lockable.")
    
        # We got the lock..
        assert lock.held() == True
        # .. so trylock should also know.
        assert lock.trylock() == False
    
        # Fcntl won't do this, but we do. Users should be able to avoid
        # relying on it.
        assert lock.test() == os.getpid()
    
        # Release the lock.
        lock.unlock()
    
        # Attempt to re-unlock should throw.
        try:
            lock.unlock()
        except AssertionError, e:
            if str(e) != WriteLock.ERROR_NOTLOCKED:
                raise
        else:
            raise AssertionError("Unlocked locks should not unlock.")

    def test_rwlocking():

        lockfile = file("/tmp/lockfile", "w+")

        fd = lockfile.fileno()

        rdlock = ReadLock(fd)
        assert rdlock.test() == None

        wrlock = WriteLock(fd)
        assert wrlock.test() == None

        rdlock.lock()
        # Same story: need to fork to get this going
        assert wrlock.test() == None
        rdlock.unlock()

        #
        # Let a child aquire it
        #

        (pin, cout) = os.pipe()
        (cin, pout) = os.pipe()

        pid = os.fork()
        if pid == 0:
            os.close(pin)
            os.close(pout)

            # Synchronize parent
            os.write(cout, "SYN")

            wrlock.lock()
            assert os.read(cin, 3) == "SYN", "Faulty parent"
            
            # Wait for parent
            assert os.read(cin, 3) == "ACK", "Faulty parent"

            sys.exit(0)

        os.close(cout)
        os.close(cin)

        # Wait for child
        assert os.read(pin, 3) == "SYN", "Faulty child"

        rdlock.lock()
        
        assert os.write(pout, "SYN")

        
    
    if __name__ == "__main__":
        print >>sys.stderr, "Running basic interface tests..."
        test_interface()
        print >>sys.stderr, "Running RW-locking stuff not clear from the manpages..."
        test_rwlocking()
        print >>sys.stderr, "OK."
