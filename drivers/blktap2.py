#!/usr/bin/env python
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
# blktap2: blktap/tapdisk management layer
#


import os
import re
import time
import copy
from lock import Lock
import util
import xmlrpclib
import httplib
import errno
import subprocess
import syslog as _syslog
import glob
import json
import xs_errors
import XenAPI
import scsiutil
from syslog import openlog, syslog
from stat import * # S_ISBLK(), ...
from SR import SROSError

import resetvdis
import vhdutil

# For RRDD Plugin Registration
from SocketServer import UnixStreamServer
from SimpleXMLRPCServer import SimpleXMLRPCDispatcher, SimpleXMLRPCRequestHandler
from xmlrpclib import ServerProxy, Fault, Transport
from socket import socket, AF_UNIX, SOCK_STREAM
from httplib import HTTP, HTTPConnection

PLUGIN_TAP_PAUSE = "tapdisk-pause"

SOCKPATH = "/var/xapi/xcp-rrdd"

NUM_PAGES_PER_RING = 32 * 11
MAX_FULL_RINGS = 8
POOL_NAME_KEY = "mem-pool"
POOL_SIZE_KEY = "mem-pool-size-rings"

ENABLE_MULTIPLE_ATTACH = "/etc/xensource/allow_multiple_vdi_attach"
NO_MULTIPLE_ATTACH = not (os.path.exists(ENABLE_MULTIPLE_ATTACH)) 

class UnixStreamHTTPConnection(HTTPConnection):
    def connect(self):
        self.sock = socket(AF_UNIX, SOCK_STREAM)
        self.sock.connect(SOCKPATH)

class UnixStreamHTTP(HTTP):
    _connection_class = UnixStreamHTTPConnection

class UnixStreamTransport(Transport):
    def make_connection(self, host):
        return UnixStreamHTTP(SOCKPATH) # overridden, but prevents IndexError


def locking(excType, override=True):
    def locking2(op):
        def wrapper(self, *args):
            self.lock.acquire()
            try:
                try:
                    ret = op(self, *args)
                except (util.CommandException, util.SMException, XenAPI.Failure), e:
                    util.logException("BLKTAP2:%s" % op)
                    msg = str(e)
                    if isinstance(e, util.CommandException):
                        msg = "Command %s failed (%s): %s" % \
                                (e.cmd, e.code, e.reason)
                    if override:
                        raise xs_errors.XenError(excType, opterr=msg)
                    else:
                        raise
                except:
                    util.logException("BLKTAP2:%s" % op)
                    raise
            finally:
                self.lock.release()
            return ret
        return wrapper
    return locking2

class RetryLoop(object):

    def __init__(self, backoff, limit):
        self.backoff = backoff
        self.limit   = limit

    def __call__(self, f):

        def loop(*__t, **__d):
            attempt = 0

            while True:
                attempt += 1

                try:
                    return f(*__t, **__d)

                except self.TransientFailure, e:
                    e = e.exception

                    if attempt >= self.limit: raise e

                    time.sleep(self.backoff)

        return loop

    class TransientFailure(Exception):
        def __init__(self, exception):
            self.exception = exception

def retried(**args): return RetryLoop(**args)

class TapCtl(object):
    """Tapdisk IPC utility calls."""

    PATH = "/usr/sbin/tap-ctl"

    def __init__(self, cmd, p):
        self.cmd    = cmd
        self._p     = p
        self.stdout = p.stdout

    class CommandFailure(Exception):
        """TapCtl cmd failure."""

        def __init__(self, cmd, **info):
            self.cmd  = cmd
            self.info = info

        def __str__(self):
            items = self.info.iteritems()
            info  = ", ".join("%s=%s" % item
                              for item in items)
            return "%s failed: %s" % (self.cmd, info)

        # Trying to get a non-existent attribute throws an AttributeError
        # exception
        def __getattr__(self, key):
            if self.info.has_key(key): return self.info[key]
            return object.__getattribute__(self, key)

        # Retrieves the error code returned by the command. If the error code
        # was not supplied at object-construction time, zero is returned.
        def get_error_code(self):
            key = 'status'
            if self.info.has_key(key):
                return self.info[key]
            else:                
                return 0

    @classmethod
    def __mkcmd_real(cls, args):
        return [ cls.PATH ] + map(str, args)

    __next_mkcmd = __mkcmd_real

    @classmethod
    def _mkcmd(cls, args):

        __next_mkcmd     = cls.__next_mkcmd
        cls.__next_mkcmd = cls.__mkcmd_real

        return __next_mkcmd(args)

    @classmethod
    def failwith(cls, status, prev=False):
        """
        Fail next invocation with @status. If @prev is true, execute
        the original command
        """

        __prev_mkcmd = cls.__next_mkcmd

        @classmethod
        def __mkcmd(cls, args):
            if prev:
                cmd = __prev_mkcmd(args)
                cmd = "'%s' && exit %d" % ("' '".join(cmd), status)
            else:
                cmd = "exit %d" % status

            return [ '/bin/sh', '-c', cmd  ]

        cls.__next_mkcmd = __mkcmd

    __strace_n = 0

    @classmethod
    def strace(cls):
        """
        Run next invocation through strace.
        Output goes to /tmp/tap-ctl.<sm-pid>.<n>; <n> counts invocations.
        """

        __prev_mkcmd = cls.__next_mkcmd

        @classmethod
        def __next_mkcmd(cls, args):

            # pylint: disable = E1101

            cmd = __prev_mkcmd(args)

            tracefile = "/tmp/%s.%d.%d" % (os.path.basename(cls.PATH),
                                           os.getpid(),
                                           cls.__strace_n)
            cls.__strace_n += 1

            return \
                [ '/usr/bin/strace', '-o', tracefile, '--'] + cmd

        cls.__next_mkcmd = __next_mkcmd

    @classmethod
    def _call(cls, args, quiet = False):
        """
        Spawn a tap-ctl process. Return a TapCtl invocation.
        Raises a TapCtl.CommandFailure if subprocess creation failed.
        """
        cmd = cls._mkcmd(args)

        if not quiet:
            util.SMlog(cmd)
        try:
            p = subprocess.Popen(cmd,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
        except OSError, e:
            raise cls.CommandFailure(cmd, errno=e.errno)

        return cls(cmd, p)

    def _errmsg(self):
        output = map(str.rstrip, self._p.stderr)
        return "; ".join(output)

    def _wait(self, quiet = False):
        """
        Reap the child tap-ctl process of this invocation.
        Raises a TapCtl.CommandFailure on non-zero exit status.
        """
        status = self._p.wait()
        if not quiet:
            util.SMlog(" = %d" % status)

        if status == 0: return

        info = { 'errmsg'   : self._errmsg(),
                 'pid'      : self._p.pid }

        if status < 0:
            info['signal'] = -status
        else:
            info['status'] = status

        raise self.CommandFailure(self.cmd, **info)

    @classmethod
    def _pread(cls, args, quiet = False):
        """
        Spawn a tap-ctl invocation and read a single line.
        """
        tapctl = cls._call(args, quiet)

        output = tapctl.stdout.readline().rstrip()

        tapctl._wait(quiet)
        return output

    @staticmethod
    def _maybe(opt, parm):
        if parm is not None: return [ opt, parm ]
        return []

    @classmethod
    def __list(cls, minor = None, pid = None, _type = None, path = None):
        args = [ "list" ]
        args += cls._maybe("-m", minor)
        args += cls._maybe("-p", pid)
        args += cls._maybe("-t", _type)
        args += cls._maybe("-f", path)

        tapctl = cls._call(args, True)

        for line in tapctl.stdout:
            # FIXME: tap-ctl writes error messages to stdout and
            # confuses this parser
            if line == "blktap kernel module not installed\n":
                # This isn't pretty but (a) neither is confusing stdout/stderr
                # and at least causes the error to describe the fix
                raise Exception, "blktap kernel module not installed: try 'modprobe blktap'"
            row = {}

            for field in line.rstrip().split(' ', 3):
                bits = field.split('=')
                if len(bits) == 2:
                    key, val = field.split('=')

                    if key in ('pid', 'minor'):
                        row[key] = int(val, 10)

                    elif key in ('state'):
                        row[key] = int(val, 0x10)

                    else:
                        row[key] = val
                else:
                    util.SMlog("Ignoring unexpected tap-ctl output: %s" % repr(field))
            yield row

        tapctl._wait(True)

    @classmethod
    @retried(backoff=.5, limit=10)
    def list(cls, **args):

        # FIXME. We typically get an EPROTO when uevents interleave
        # with SM ops and a tapdisk shuts down under our feet. Should
        # be fixed in SM.

        try:
            return list(cls.__list(**args))

        except cls.CommandFailure, e:
            transient = [ errno.EPROTO, errno.ENOENT ]
            if e.status in transient:
                raise RetryLoop.TransientFailure(e)
            raise

    @classmethod
    def allocate(cls, devpath = None):
        args = [ "allocate" ]
        args += cls._maybe("-d", devpath)
        return cls._pread(args)

    @classmethod
    def free(cls, minor):
        args = [ "free", "-m", minor ]
        cls._pread(args)

    @classmethod
    def spawn(cls):
        args = [ "spawn" ]
        pid = cls._pread(args)
        return int(pid)

    @classmethod
    def attach(cls, pid, minor):
        args = [ "attach", "-p", pid, "-m", minor ]
        cls._pread(args)

    @classmethod
    def detach(cls, pid, minor):
        args = [ "detach", "-p", pid, "-m", minor ]
        cls._pread(args)

    @classmethod
    def open(cls, pid, minor, _type, _file, options):
        params = Tapdisk.Arg(_type, _file)
        args = [ "open", "-p", pid, "-m", minor, '-a', str(params) ]
        if options.get("rdonly"):
            args.append('-R')
        if options.get("lcache"):
            args.append("-r")
        if options.get("existing_prt") != None:
            args.append("-e")
            args.append(str(options["existing_prt"]))
        if options.get("secondary"):
            args.append("-2")
            args.append(options["secondary"])
        if options.get("standby"):
            args.append("-s")
        if options.get("timeout"):
            args.append("-t")
            args.append(str(options["timeout"]))
        cls._pread(args)

    @classmethod
    def close(cls, pid, minor, force = False):
        args = [ "close", "-p", pid, "-m", minor ]
        if force: args += [ "-f" ]
        cls._pread(args)

    @classmethod
    def pause(cls, pid, minor):
        args = [ "pause", "-p", pid, "-m", minor ]
        cls._pread(args)

    @classmethod
    def unpause(cls, pid, minor, _type = None, _file = None, mirror = None):
        args = [ "unpause", "-p", pid, "-m", minor ]
        if mirror:
            args.extend(["-2", mirror])
        if _type and _file:
            params = Tapdisk.Arg(_type, _file)
            args  += [ "-a", str(params) ]
        cls._pread(args)

    @classmethod
    def stats(cls, pid, minor):
        args = [ "stats", "-p", pid, "-m", minor ]
        return cls._pread(args, quiet = True)

    @classmethod
    def major(cls):
        args = [ "major" ]
        major = cls._pread(args)
        return int(major)

class TapdiskExists(Exception):
    """Tapdisk already running."""

    def __init__(self, tapdisk):
        self.tapdisk = tapdisk

    def __str__(self):
        return "%s already running" % self.tapdisk

class TapdiskNotRunning(Exception):
    """No such Tapdisk."""

    def __init__(self, **attrs):
        self.attrs = attrs

    def __str__(self):
        items = self.attrs.iteritems()
        attrs = ", ".join("%s=%s" % attr
                          for attr in items)
        return "No such Tapdisk(%s)" % attrs

class TapdiskNotUnique(Exception):
    """More than one tapdisk on one path."""

    def __init__(self, tapdisks):
        self.tapdisks = tapdisks

    def __str__(self):
        tapdisks = map(str, self.tapdisks)
        return "Found multiple tapdisks: %s" % tapdisks

class TapdiskFailed(Exception):
    """Tapdisk launch failure."""

    def __init__(self, arg, err):
        self.arg   = arg
        self.err   = err

    def __str__(self):
        return "Tapdisk(%s): %s" % (self.arg, self.err)

    def get_error(self):
        return self.err

class TapdiskInvalidState(Exception):
    """Tapdisk pause/unpause failure"""

    def __init__(self, tapdisk):
        self.tapdisk = tapdisk

    def __str__(self):
        return str(self.tapdisk)

def mkdirs(path, mode=0777):
    if not os.path.exists(path):
        parent, subdir = os.path.split(path)
        assert parent != path
        try:
            if parent:
                mkdirs(parent, mode)
            if subdir:
                os.mkdir(path, mode)
        except OSError, e:
            if e.errno != errno.EEXIST:
                raise

class KObject(object):

    SYSFS_CLASSTYPE = None

    def sysfs_devname(self):
        raise NotImplementedError("sysfs_devname is undefined")

class Attribute(object):

    SYSFS_NODENAME = None

    def __init__(self, path):
        self.path = path

    @classmethod
    def from_kobject(cls, kobj):
        path = "%s/%s" % (kobj.sysfs_path(), cls.SYSFS_NODENAME)
        return cls(path)

    class NoSuchAttribute(Exception):
        def __init__(self, name):
            self.name = name

        def __str__(self):
            return "No such attribute: %s" % self.name

    def _open(self, mode='r'):
        try:
            return file(self.path, mode)
        except IOError, e:
            if e.errno == errno.ENOENT:
                raise self.NoSuchAttribute(self)
            raise

    def readline(self):
        f = self._open('r')
        s = f.readline().rstrip()
        f.close()
        return s

    def writeline(self, val):
        f = self._open('w')
        f.write(val)
        f.close()

class ClassDevice(KObject):

    @classmethod
    def sysfs_class_path(cls):
        return "/sys/class/%s" % cls.SYSFS_CLASSTYPE

    def sysfs_path(self):
        return "%s/%s" % (self.sysfs_class_path(),
                          self.sysfs_devname())

class Blktap(ClassDevice):

    DEV_BASEDIR = '/dev/xen/blktap-2'

    SYSFS_CLASSTYPE = "blktap2"

    def __init__(self, minor):
        self.minor = minor
        self._pool = None
        self._task = None

    @classmethod
    def allocate(cls):
        # FIXME. Should rather go into init.
        mkdirs(cls.DEV_BASEDIR)

        devname = TapCtl.allocate()
        minor   = Tapdisk._parse_minor(devname)
        return cls(minor)

    def free(self):
        TapCtl.free(self.minor)

    def __str__(self):
        return "%s(minor=%d)" % (self.__class__.__name__, self.minor)

    def sysfs_devname(self):
        return "blktap%d" % self.minor

    class Pool(Attribute):
        SYSFS_NODENAME = "pool"

    def get_pool_attr(self):
        if not self._pool:
            self._pool = self.Pool.from_kobject(self)
        return self._pool

    def get_pool_name(self):
        return self.get_pool_attr().readline()

    def set_pool_name(self, name):
        self.get_pool_attr().writeline(name)

    def set_pool_size(self, pages):
        self.get_pool().set_size(pages)

    def get_pool(self):
        return BlktapControl.get_pool(self.get_pool_name())

    def set_pool(self, pool):
        self.set_pool_name(pool.name)

    class Task(Attribute):
        SYSFS_NODENAME = "task"

    def get_task_attr(self):
        if not self._task:
            self._task = self.Task.from_kobject(self)
        return self._task

    def get_task_pid(self):
        pid = self.get_task_attr().readline()
        try:
            return int(pid)
        except ValueError:
            return None

    def find_tapdisk(self):
        pid = self.get_task_pid()
        if pid is None: return None

        return Tapdisk.find(pid=pid, minor=self.minor)

    def get_tapdisk(self):
        tapdisk = self.find_tapdisk()
        if not tapdisk:
            raise TapdiskNotRunning(minor=self.minor)
        return tapdisk

class Tapdisk(object):

    TYPES = [ 'aio', 'vhd' ]

    def __init__(self, pid, minor, _type, path, state):
        self.pid     = pid
        self.minor   = minor
        self.type    = _type
        self.path    = path
        self.state   = state
        self._dirty  = False
        self._blktap = None

    def __str__(self):
        state = self.pause_state()
        return "Tapdisk(%s, pid=%d, minor=%s, state=%s)" % \
            (self.get_arg(), self.pid, self.minor, state)

    @classmethod
    def list(cls, **args):

        for row in TapCtl.list(**args):

            args =  { 'pid'     : None,
                      'minor'   : None,
                      'state'   : None,
                      '_type'   : None,
                      'path'    : None }

            for key, val in row.iteritems():
                if key in args:
                    args[key] = val

            if 'args' in row:
                image = Tapdisk.Arg.parse(row['args'])
                args['_type'] = image.type
                args['path']  = image.path

            if None in args.values():
                continue

            yield Tapdisk(**args)

    @classmethod
    def find(cls, **args):

        found = list(cls.list(**args))

        if len(found) > 1:
            raise TapdiskNotUnique(found)

        if found:
            return found[0]

        return None

    @classmethod
    def find_by_path(cls, path):
        return cls.find(path=path)

    @classmethod
    def find_by_minor(cls, minor):
        return cls.find(minor=minor)

    @classmethod
    def get(cls, **attrs):

        tapdisk = cls.find(**attrs)

        if not tapdisk:
            raise TapdiskNotRunning(**attrs)

        return tapdisk

    @classmethod
    def from_path(cls, path):
        return cls.get(path=path)

    @classmethod
    def from_minor(cls, minor):
        return cls.get(minor=minor)

    @classmethod
    def __from_blktap(cls, blktap):
        tapdisk = cls.from_minor(minor=blktap.minor)
        tapdisk._blktap = blktap
        return tapdisk

    def get_blktap(self):
        if not self._blktap:
            self._blktap = Blktap(self.minor)
        return self._blktap

    class Arg:

        def __init__(self, _type, path):
            self.type = _type
            self.path =  path

        def __str__(self):
            return "%s:%s" % (self.type, self.path)

        @classmethod
        def parse(cls, arg):

            try:
                _type, path = arg.split(":", 1)
            except ValueError:
                raise cls.InvalidArgument(arg)

            if _type not in Tapdisk.TYPES:
                raise cls.InvalidType(_type, path)

            return cls(_type, path)

        class InvalidType(Exception):
            def __init__(self, _type):
                self.type = _type

            def __str__(self):
                return "Not a Tapdisk type: %s" % self.type

        class InvalidArgument(Exception):
            def __init__(self, arg):
                self.arg = arg

            def __str__(self):
                return "Not a Tapdisk image: %s" % self.arg

    def get_arg(self):
        return self.Arg(self.type, self.path)

    def get_devpath(self):
        return "%s/tapdev%d" % (Blktap.DEV_BASEDIR, self.minor)

    @classmethod
    def launch_from_arg(cls, arg):
        arg = cls.Arg.parse(arg)
        return cls.launch(arg.path, arg.type, False)

    @classmethod
    def launch_on_tap(cls, blktap, path, _type, options):

        tapdisk = cls.find_by_path(path)
        if tapdisk:
            raise TapdiskExists(tapdisk)

        minor = blktap.minor

        try:
            pid = TapCtl.spawn()

            try:
                TapCtl.attach(pid, minor)

                try:
                    TapCtl.open(pid, minor, _type, path, options)
                    try:
                        return cls.__from_blktap(blktap)

                    except:
                        TapCtl.close(pid, minor)
                        raise

                except:
                    TapCtl.detach(pid, minor)
                    raise

            except:
                # FIXME: Should be tap-ctl shutdown.
                try:
                    import signal
                    os.kill(pid, signal.SIGTERM)
                    os.waitpid(pid, 0)
                finally:
                    raise

        except TapCtl.CommandFailure, ctl:
            util.logException(ctl)
            raise TapdiskFailed(cls.Arg(_type, path), ctl)

    @classmethod
    def launch(cls, path, _type, rdonly):
        blktap = Blktap.allocate()
        try:
            return cls.launch_on_tap(blktap, path, _type, {"rdonly": rdonly})
        except:
            blktap.free()
            raise

    def shutdown(self, force = False):

        TapCtl.close(self.pid, self.minor, force)

        try:
            util.SMlog('Attempt to deregister tapdisk with RRDD.')
            pluginName = "tap" + str(self.pid) + "-" + str(self.minor)
            proxy = ServerProxy('http://' + SOCKPATH, transport=UnixStreamTransport())
            proxy.Plugin.deregister({'uid': pluginName})
        except Exception, e:
            util.SMlog('ERROR: Failed to deregister tapdisk with RRDD due to %s' % e)

        TapCtl.detach(self.pid, self.minor)

        self.get_blktap().free()

    def pause(self):

        if not self.is_running():
            raise TapdiskInvalidState(self)

        TapCtl.pause(self.pid, self.minor)

        self._set_dirty()

    def unpause(self, _type=None, path=None, mirror=None):

        if not self.is_paused():
            raise TapdiskInvalidState(self)

        # FIXME: should the arguments be optional?
        if _type is None: _type = self.type
        if  path is None:  path = self.path

        TapCtl.unpause(self.pid, self.minor, _type, path, mirror=mirror)

        self._set_dirty()

    def stats(self):
        json = TapCtl.stats(self.pid, self.minor)
        return json.loads(json)

    #
    # NB. dirty/refresh: reload attributes on next access
    #

    def _set_dirty(self):
        self._dirty = True

    def _refresh(self, __get):
        t = self.from_minor(__get('minor'))
        self.__init__(t.pid, t.minor, t.type, t.path, t.state)

    def __getattribute__(self, name):
        def __get(name):
            # NB. avoid(rec(ursion)
            return object.__getattribute__(self, name)

        if __get('_dirty') and \
                name in ['minor', 'type', 'path', 'state']:
            self._refresh(__get)
            self._dirty = False

        return __get(name)

    class PauseState:
        RUNNING             = 'R'
        PAUSING             = 'r'
        PAUSED              = 'P'

    class Flags:
        DEAD                 = 0x0001
        CLOSED               = 0x0002
        QUIESCE_REQUESTED    = 0x0004
        QUIESCED             = 0x0008
        PAUSE_REQUESTED      = 0x0010
        PAUSED               = 0x0020
        SHUTDOWN_REQUESTED   = 0x0040
        LOCKING              = 0x0080
        RETRY_NEEDED         = 0x0100
        LOG_DROPPED          = 0x0200

        PAUSE_MASK           = PAUSE_REQUESTED|PAUSED

    def is_paused(self):
        return not not (self.state & self.Flags.PAUSED)

    def is_running(self):
        return not (self.state & self.Flags.PAUSE_MASK)

    def pause_state(self):
        if self.state & self.Flags.PAUSED:
            return self.PauseState.PAUSED

        if self.state & self.Flags.PAUSE_REQUESTED:
            return self.PauseState.PAUSING

        return self.PauseState.RUNNING

    @staticmethod
    def _parse_minor(devpath):

        regex   = '%s/(blktap|tapdev)(\d+)$' % Blktap.DEV_BASEDIR
        pattern = re.compile(regex)
        groups  = pattern.search(devpath)
        if not groups:
            raise Exception, \
                "malformed tap device: '%s' (%s) " % (devpath, regex)

        minor = groups.group(2)
        return int(minor)

    _major = None

    @classmethod
    def major(cls):
        if cls._major: return cls._major

        devices = file("/proc/devices")
        for line in devices:

            row = line.rstrip().split(' ')
            if len(row) != 2: continue

            major, name = row
            if name != 'tapdev': continue

            cls._major = int(major)
            break

        devices.close()
        return cls._major

class VDI(object):
    """SR.vdi driver decorator for blktap2"""

    CONF_KEY_ALLOW_CACHING = "vdi_allow_caching"
    CONF_KEY_MODE_ON_BOOT = "vdi_on_boot"
    CONF_KEY_CACHE_SR = "local_cache_sr"
    LOCK_CACHE_SETUP = "cachesetup"

    ATTACH_DETACH_RETRY_SECS = 120

    # number of seconds on top of NFS timeo mount option the tapdisk should 
    # wait before reporting errors. This is to allow a retry to succeed in case 
    # packets were lost the first time around, which prevented the NFS client 
    # from returning before the timeo is reached even if the NFS server did 
    # come back earlier
    TAPDISK_TIMEOUT_MARGIN = 30

    def __init__(self, uuid, target, driver_info):
        self.target      = self.TargetDriver(target, driver_info)
        self._vdi_uuid   = uuid
        self._session    = target.session
        self.xenstore_data = scsiutil.update_XS_SCSIdata(uuid,scsiutil.gen_synthetic_page_data(uuid))
        self.lock        = Lock("vdi", uuid)

    @classmethod
    def from_cli(cls, uuid):
        import VDI as sm
        import XenAPI

        session = XenAPI.xapi_local()
        session.xenapi.login_with_password('root', '')

        target = sm.VDI.from_uuid(session, uuid)
        driver_info = target.sr.srcmd.driver_info

        return cls(uuid, target, driver_info)

    @staticmethod
    def _tap_type(vdi_type):
        """Map a VDI type (e.g. 'raw') to a tapdisk driver type (e.g. 'aio')"""
        return {
            'raw'  : 'aio',
            'vhd'  : 'vhd',
            'iso'  : 'aio', # for ISO SR
            'aio'  : 'aio', # for LVHD
            'file' : 'aio',
            } [vdi_type]

    def get_tap_type(self):
        vdi_type = self.target.get_vdi_type()
        return VDI._tap_type(vdi_type)

    def get_phy_path(self):
        return self.target.get_vdi_path()

    class UnexpectedVDIType(Exception):

        def __init__(self, vdi_type, target):
            self.vdi_type = vdi_type
            self.target   = target

        def __str__(self):
            return \
                "Target %s has unexpected VDI type '%s'" % \
                (type(self.target), self.vdi_type)

    VDI_PLUG_TYPE = { 'phy'  : 'phy',  # for NETAPP
                      'raw'  : 'phy',
                      'aio'  : 'tap',  # for LVHD raw nodes
                      'iso'  : 'tap', # for ISOSR
                      'file' : 'tap',
                      'vhd'  : 'tap' }

    def tap_wanted(self):

        # 1. Let the target vdi_type decide

        vdi_type = self.target.get_vdi_type()

        try:
            plug_type = self.VDI_PLUG_TYPE[vdi_type]
        except KeyError:
            raise self.UnexpectedVDIType(vdi_type,
                                         self.target.vdi)

        if plug_type == 'tap': return True

        # 2. Otherwise, there may be more reasons
        #
        # .. TBD

        return False

    class TargetDriver:
        """Safe target driver access."""

        # NB. *Must* test caps for optional calls. Some targets
        # actually implement some slots, but do not enable them. Just
        # try/except would risk breaking compatibility.

        def __init__(self, vdi, driver_info):
            self.vdi    = vdi
            self._caps  = driver_info['capabilities']

        def has_cap(self, cap):
            """Determine if target has given capability"""
            return cap in self._caps

        def attach(self, sr_uuid, vdi_uuid):
            #assert self.has_cap("VDI_ATTACH")
            return self.vdi.attach(sr_uuid, vdi_uuid)

        def detach(self, sr_uuid, vdi_uuid):
            #assert self.has_cap("VDI_DETACH")
            self.vdi.detach(sr_uuid, vdi_uuid)

        def activate(self, sr_uuid, vdi_uuid):
            if self.has_cap("VDI_ACTIVATE"):
                self.vdi.activate(sr_uuid, vdi_uuid)

        def deactivate(self, sr_uuid, vdi_uuid):
            if self.has_cap("VDI_DEACTIVATE"):
                self.vdi.deactivate(sr_uuid, vdi_uuid)

        #def resize(self, sr_uuid, vdi_uuid, size):
        #    return self.vdi.resize(sr_uuid, vdi_uuid, size)

        def get_vdi_type(self):
            _type = self.vdi.vdi_type
            if not _type:
                _type = self.vdi.sr.sr_vditype
            if not _type:
                raise VDI.UnexpectedVDIType(_type, self.vdi)
            return _type

        def get_vdi_path(self):
            return self.vdi.path

    class Link(object):
        """Relink a node under a common name"""

        # NB. We have to provide the device node path during
        # VDI.attach, but currently do not allocate the tapdisk minor
        # before VDI.activate. Therefore those link steps where we
        # relink existing devices under deterministic path names.

        BASEDIR = None

        def _mklink(self, target):
            raise NotImplementedError("_mklink is not defined")

        def _equals(self, target):
            raise NotImplementedError("_equals is not defined")

        def __init__(self, path):
            self._path = path

        @classmethod
        def from_name(cls, name):
            path = "%s/%s" % (cls.BASEDIR, name)
            return cls(path)

        @classmethod
        def from_uuid(cls, sr_uuid, vdi_uuid):
            name = "%s/%s" % (sr_uuid, vdi_uuid)
            return cls.from_name(name)

        def path(self):
            return self._path

        def stat(self):
            return os.stat(self.path())

        def mklink(self, target):

            path = self.path()
            util.SMlog("%s -> %s" % (self, target))

            mkdirs(os.path.dirname(path))
            try:
                self._mklink(target)
            except OSError, e:
                # We do unlink during teardown, but have to stay
                # idempotent. However, a *wrong* target should never
                # be seen.
                if e.errno != errno.EEXIST: raise
                assert self._equals(target)

        def unlink(self):
            try:
                os.unlink(self.path())
            except OSError, e:
                if e.errno != errno.ENOENT: raise

        def __str__(self):
            path = self.path()
            return "%s(%s)" % (self.__class__.__name__, path)

    class SymLink(Link):
        """Symlink some file to a common name"""

        def readlink(self):
            return os.readlink(self.path())

        def symlink(self):
            return self.path

        def _mklink(self, target):
            os.symlink(target, self.path())

        def _equals(self, target):
            return self.readlink() == target

    class DeviceNode(Link):
        """Relink a block device node to a common name"""

        @classmethod
        def _real_stat(cls, target):
            """stat() not on @target, but its realpath()"""
            _target = os.path.realpath(target)
            return os.stat(_target)

        @classmethod
        def is_block(cls, target):
            """Whether @target refers to a block device."""
            return S_ISBLK(cls._real_stat(target).st_mode)

        def _mklink(self, target):

            st = self._real_stat(target)
            if not S_ISBLK(st.st_mode):
                raise self.NotABlockDevice(target, st)

            os.mknod(self.path(), st.st_mode, st.st_rdev)

        def _equals(self, target):
            target_rdev = self._real_stat(target).st_rdev
            return self.stat().st_rdev == target_rdev

        def rdev(self):
            st = self.stat()
            assert S_ISBLK(st.st_mode)
            return os.major(st.st_rdev), os.minor(st.st_rdev)

        class NotABlockDevice(Exception):

            def __init__(self, path, st):
                self.path = path
                self.st   = st

            def __str__(self):
                return "%s is not a block device: %s" % (self.path, self.st)

    class Hybrid(Link):

        def __init__(self, path):
            VDI.Link.__init__(self, path)
            self._devnode = VDI.DeviceNode(path)
            self._symlink = VDI.SymLink(path)

        def rdev(self):
            st = self.stat()
            if S_ISBLK(st.st_mode): return self._devnode.rdev()
            raise self._devnode.NotABlockDevice(self.path(), st)

        def mklink(self, target):
            if self._devnode.is_block(target):
                self._obj = self._devnode
            else:
                self._obj = self._symlink
            self._obj.mklink(target)

        def _equals(self, target):
            return self._obj._equals(target)

    class PhyLink(SymLink): BASEDIR = "/dev/sm/phy"
    # NB. Cannot use DeviceNodes, e.g. FileVDIs aren't bdevs.

    class BackendLink(Hybrid): BASEDIR = "/dev/sm/backend"
    # NB. Could be SymLinks as well, but saving major,minor pairs in
    # Links enables neat state capturing when managing Tapdisks.  Note
    # that we essentially have a tap-ctl list replacement here. For
    # now make it a 'Hybrid'. Likely to collapse into a DeviceNode as
    # soon as ISOs are tapdisks.

    @staticmethod
    def _tap_activate(phy_path, vdi_type, sr_uuid, options, pool_size = None):

        tapdisk = Tapdisk.find_by_path(phy_path)
        if not tapdisk:
            blktap = Blktap.allocate()
            blktap.set_pool_name(sr_uuid)
            if pool_size:
                blktap.set_pool_size(pool_size)

            try:
                tapdisk = \
                    Tapdisk.launch_on_tap(blktap,
                                          phy_path,
                                          VDI._tap_type(vdi_type),
                                          options)
            except:
                blktap.free()
                raise
            util.SMlog("tap.activate: Launched %s" % tapdisk)
            #Register tapdisk as rrdd plugin
            try:
                util.SMlog('Attempt to register tapdisk with RRDD as a plugin.')
                pluginName = "tap-" + str(tapdisk.pid) + "-" + str(tapdisk.minor)
                proxy = ServerProxy('http://' + SOCKPATH, transport=UnixStreamTransport())
                proxy.Plugin.register({'uid': pluginName, 'frequency': 'Five_seconds'})
            except Exception, e:
                util.SMlog('ERROR: Failed to register tapdisk with RRDD due to %s' % e)
        else:
            util.SMlog("tap.activate: Found %s" % tapdisk)

        return tapdisk.get_devpath()

    @staticmethod
    def _tap_deactivate(minor):

        try:
            tapdisk = Tapdisk.from_minor(minor)
        except TapdiskNotRunning, e:
            util.SMlog("tap.deactivate: Warning, %s" % e)
            # NB. Should not be here unless the agent refcount
            # broke. Also, a clean shutdown should not have leaked
            # the recorded minor.
        else:
            tapdisk.shutdown()
            util.SMlog("tap.deactivate: Shut down %s" % tapdisk)

    @classmethod
    def tap_pause(cls, session, sr_uuid, vdi_uuid):
        util.SMlog("Pause request for %s" % vdi_uuid)
        vdi_ref = session.xenapi.VDI.get_by_uuid(vdi_uuid)
        session.xenapi.VDI.add_to_sm_config(vdi_ref, 'paused', 'true')
        sm_config = session.xenapi.VDI.get_sm_config(vdi_ref)
        for key in filter(lambda x: x.startswith('host_'), sm_config.keys()):
            host_ref = key[len('host_'):]
            util.SMlog("Calling tap-pause on host %s" % host_ref)
            if not cls.call_pluginhandler(session, host_ref,
                    sr_uuid, vdi_uuid, "pause"):
                # Failed to pause node
                session.xenapi.VDI.remove_from_sm_config(vdi_ref, 'paused')
                return False
        return True

    @classmethod
    def tap_unpause(cls, session, sr_uuid, vdi_uuid, secondary = None,
            activate_parents = False):
        util.SMlog("Unpause request for %s secondary=%s" % (vdi_uuid, secondary))
        vdi_ref = session.xenapi.VDI.get_by_uuid(vdi_uuid)
        sm_config = session.xenapi.VDI.get_sm_config(vdi_ref)
        for key in filter(lambda x: x.startswith('host_'), sm_config.keys()):
            host_ref = key[len('host_'):]
            util.SMlog("Calling tap-unpause on host %s" % host_ref)
            if not cls.call_pluginhandler(session, host_ref,
                    sr_uuid, vdi_uuid, "unpause", secondary, activate_parents):
                # Failed to unpause node
                return False
        session.xenapi.VDI.remove_from_sm_config(vdi_ref, 'paused')
        return True

    @classmethod
    def tap_refresh(cls, session, sr_uuid, vdi_uuid, activate_parents = False):
        util.SMlog("Refresh request for %s" % vdi_uuid)
        vdi_ref = session.xenapi.VDI.get_by_uuid(vdi_uuid)
        sm_config = session.xenapi.VDI.get_sm_config(vdi_ref)
        for key in filter(lambda x: x.startswith('host_'), sm_config.keys()):
            host_ref = key[len('host_'):]
            util.SMlog("Calling tap-refresh on host %s" % host_ref)
            if not cls.call_pluginhandler(session, host_ref,
                    sr_uuid, vdi_uuid, "refresh", None, activate_parents):
                # Failed to refresh node
                return False
        return True

    @classmethod
    def call_pluginhandler(cls, session, host_ref, sr_uuid, vdi_uuid, action,
            secondary = None, activate_parents = False):
        """Optionally, activate the parent LV before unpausing"""
        try:
            args = {"sr_uuid":sr_uuid,"vdi_uuid":vdi_uuid}
            if secondary:
                args["secondary"] = secondary
            if activate_parents:
                args["activate_parents"] = "true"
            ret = session.xenapi.host.call_plugin(
                    host_ref, PLUGIN_TAP_PAUSE, action,
                    args)
            return ret == "True"
        except:
            util.logException("BLKTAP2:call_pluginhandler")
            return False


    def _add_tag(self, vdi_uuid, writable):
        util.SMlog("Adding tag to: %s" % vdi_uuid)
        attach_mode = "RO"
        if writable:
            attach_mode = "RW"
        vdi_ref = self._session.xenapi.VDI.get_by_uuid(vdi_uuid)
        host_ref = self._session.xenapi.host.get_by_uuid(util.get_this_host())
        sm_config = self._session.xenapi.VDI.get_sm_config(vdi_ref)
        attached_as = util.attached_as(sm_config)
        if NO_MULTIPLE_ATTACH and (attached_as == "RW" or \
                (attached_as == "RO" and attach_mode == "RW")):
            util.SMlog("need to reset VDI %s" % vdi_uuid)
            if not resetvdis.reset_vdi(self._session, vdi_uuid, force=False,
                    term_output=False, writable=writable):
                raise util.SMException("VDI %s not detached cleanly" % vdi_uuid)
            sm_config = self._session.xenapi.VDI.get_sm_config(vdi_ref)
        if sm_config.has_key('paused'):
            util.SMlog("Paused or host_ref key found [%s]" % sm_config)
            return False
        host_key = "host_%s" % host_ref
        if sm_config.has_key(host_key):
            util.SMlog("WARNING: host key %s (%s) already there!" % (host_key,
                    sm_config[host_key]))
        else:
            self._session.xenapi.VDI.add_to_sm_config(vdi_ref, host_key,
                    attach_mode)
        sm_config = self._session.xenapi.VDI.get_sm_config(vdi_ref)
        if sm_config.has_key('paused'):
            util.SMlog("Found paused key, aborting")
            self._session.xenapi.VDI.remove_from_sm_config(vdi_ref, host_key)
            return False
        util.SMlog("Activate lock succeeded")
        return True

    def _check_tag(self, vdi_uuid):
        vdi_ref = self._session.xenapi.VDI.get_by_uuid(vdi_uuid)
        sm_config = self._session.xenapi.VDI.get_sm_config(vdi_ref)
        if sm_config.has_key('paused'):
            util.SMlog("Paused key found [%s]" % sm_config)
            return False
        return True

    def _remove_tag(self, vdi_uuid):
        vdi_ref = self._session.xenapi.VDI.get_by_uuid(vdi_uuid)
        host_ref = self._session.xenapi.host.get_by_uuid(util.get_this_host())
        sm_config = self._session.xenapi.VDI.get_sm_config(vdi_ref)
        host_key = "host_%s" % host_ref
        if sm_config.has_key(host_key):
            self._session.xenapi.VDI.remove_from_sm_config(vdi_ref, host_key)
            util.SMlog("Removed host key %s for %s" % (host_key, vdi_uuid))
        else:
            util.SMlog("WARNING: host key %s not found!" % host_key)

    def _get_pool_config(self, pool_name):
        pool_info = dict()
        vdi_ref = self.target.vdi.sr.srcmd.params.get('vdi_ref')
        if not vdi_ref:
            # attach_from_config context: HA disks don't need to be in any 
            # special pool
            return pool_info
        session = XenAPI.xapi_local()
        session.xenapi.login_with_password('root', '')
        sr_ref = self.target.vdi.sr.srcmd.params.get('sr_ref')
        sr_config = session.xenapi.SR.get_other_config(sr_ref)
        vdi_config = session.xenapi.VDI.get_other_config(vdi_ref)
        pool_size_str = sr_config.get(POOL_SIZE_KEY)
        pool_name_override = vdi_config.get(POOL_NAME_KEY)
        if pool_name_override:
            pool_name = pool_name_override
            pool_size_override = vdi_config.get(POOL_SIZE_KEY)
            if pool_size_override:
                pool_size_str = pool_size_override
        pool_size = 0
        if pool_size_str:
            try:
                pool_size = int(pool_size_str)
                if pool_size < 1 or pool_size > MAX_FULL_RINGS:
                    raise ValueError("outside of range")
                pool_size = NUM_PAGES_PER_RING * pool_size
            except ValueError:
                util.SMlog("Error: invalid mem-pool-size %s" % pool_size_str)
                pool_size = 0

        pool_info["mem-pool"] = pool_name
        if pool_size:
            pool_info["mem-pool-size"] = str(pool_size)

        session.xenapi.session.logout()
        return pool_info

    def attach(self, sr_uuid, vdi_uuid, writable, activate = False):
        """Return/dev/sm/backend symlink path"""
        self.xenstore_data.update(self._get_pool_config(sr_uuid))
        if not self.target.has_cap("ATOMIC_PAUSE") or activate:
            util.SMlog("Attach & activate")
            self._attach(sr_uuid, vdi_uuid)
            dev_path = self._activate(sr_uuid, vdi_uuid,
                    {"rdonly": not writable})
            self.BackendLink.from_uuid(sr_uuid, vdi_uuid).mklink(dev_path)

        # Return backend/ link
        back_path = self.BackendLink.from_uuid(sr_uuid, vdi_uuid).path()
        struct = { 'params': back_path,
                   'xenstore_data': self.xenstore_data}
        util.SMlog('result: %s' % struct)

	try:
            f=open("%s.attach_info" % back_path, 'a')
            f.write(xmlrpclib.dumps((struct,), "", True))
            f.close()
	except:
	    pass

        return xmlrpclib.dumps((struct,), "", True)

    def activate(self, sr_uuid, vdi_uuid, writable, caching_params):
        util.SMlog("blktap2.activate")
        options = {"rdonly": not writable}
        options.update(caching_params)
        timeout = util.get_nfs_timeout(self.target.vdi.session, sr_uuid)
        if timeout:
            options["timeout"] = timeout + self.TAPDISK_TIMEOUT_MARGIN
        for i in range(self.ATTACH_DETACH_RETRY_SECS):
            try:
                if self._activate_locked(sr_uuid, vdi_uuid, options):
                    return
            except util.SRBusyException:
                util.SMlog("SR locked, retrying")
            time.sleep(1)
        raise util.SMException("VDI %s locked" % vdi_uuid)

    @locking("VDIUnavailable")
    def _activate_locked(self, sr_uuid, vdi_uuid, options):
        """Wraps target.activate and adds a tapdisk"""
        import VDI as sm

        #util.SMlog("VDI.activate %s" % vdi_uuid)
        if self.tap_wanted():
            if not self._add_tag(vdi_uuid, not options["rdonly"]):
                return False
            # it is possible that while the VDI was paused some of its 
            # attributes have changed (e.g. its size if it was inflated; or its 
            # path if it was leaf-coalesced onto a raw LV), so refresh the 
            # object completely
            params = self.target.vdi.sr.srcmd.params
            target = sm.VDI.from_uuid(self.target.vdi.session, vdi_uuid)
            target.sr.srcmd.params = params
            driver_info = target.sr.srcmd.driver_info
            self.target = self.TargetDriver(target, driver_info)

        try:
            util.fistpoint.activate_custom_fn(
                    "blktap_activate_inject_failure",
                    lambda: util.inject_failure())

            # Attach the physical node
            if self.target.has_cap("ATOMIC_PAUSE"):
                self._attach(sr_uuid, vdi_uuid)

            # Activate the physical node
            dev_path = self._activate(sr_uuid, vdi_uuid, options)
        except:
            util.SMlog("Exception in activate/attach")
            if self.tap_wanted():
                util.fistpoint.activate_custom_fn(
                        "blktap_activate_error_handling",
                        lambda: time.sleep(30))
                while True:
                    try:
                        self._remove_tag(vdi_uuid)
                        break
                    except xmlrpclib.ProtocolError, e:
                        # If there's a connection error, keep trying forever.
                        if e.errcode == httplib.INTERNAL_SERVER_ERROR:
                            continue
                        else:
                            util.SMlog('failed to remove tag: %s' % e)
                            break
                    except Exception, e:
                        util.SMlog('failed to remove tag: %s' % e)
                        break
            raise

        # Link result to backend/
        self.BackendLink.from_uuid(sr_uuid, vdi_uuid).mklink(dev_path)
        return True
    
    def _activate(self, sr_uuid, vdi_uuid, options):
        self.target.activate(sr_uuid, vdi_uuid)

        dev_path = self.setup_cache(sr_uuid, vdi_uuid, options)
        if not dev_path:
            phy_path = self.PhyLink.from_uuid(sr_uuid, vdi_uuid).readlink()
            # Maybe launch a tapdisk on the physical link
            if self.tap_wanted():
                vdi_type = self.target.get_vdi_type()
                dev_path = self._tap_activate(phy_path, vdi_type, sr_uuid,
                        options,
                        self._get_pool_config(sr_uuid).get("mem-pool-size"))
            else:
                dev_path = phy_path # Just reuse phy

        return dev_path

    def _attach(self, sr_uuid, vdi_uuid):
        attach_info = xmlrpclib.loads(self.target.attach(sr_uuid, vdi_uuid))[0][0]
        params = attach_info['params']
        xenstore_data = attach_info['xenstore_data']
        phy_path = util.to_plain_string(params)
        self.xenstore_data.update(xenstore_data)
        # Save it to phy/
        self.PhyLink.from_uuid(sr_uuid, vdi_uuid).mklink(phy_path)

    def deactivate(self, sr_uuid, vdi_uuid, caching_params):
        util.SMlog("blktap2.deactivate")
        for i in range(self.ATTACH_DETACH_RETRY_SECS):
            try:
                if self._deactivate_locked(sr_uuid, vdi_uuid, caching_params):
                    return
            except util.SRBusyException, e:
                util.SMlog("SR locked, retrying")
            time.sleep(1)
        raise util.SMException("VDI %s locked" % vdi_uuid)

    @locking("VDIUnavailable")
    def _deactivate_locked(self, sr_uuid, vdi_uuid, caching_params):
        """Wraps target.deactivate and removes a tapdisk"""

        #util.SMlog("VDI.deactivate %s" % vdi_uuid)
        if self.tap_wanted() and not self._check_tag(vdi_uuid):
            return False

        self._deactivate(sr_uuid, vdi_uuid, caching_params)
        if self.target.has_cap("ATOMIC_PAUSE"):
            self._detach(sr_uuid, vdi_uuid)
        if self.tap_wanted():
            self._remove_tag(vdi_uuid)

        return True

    def _resetPhylink(self, sr_uuid, vdi_uuid, path):
        self.PhyLink.from_uuid(sr_uuid, vdi_uuid).mklink(path)

    def detach(self, sr_uuid, vdi_uuid):
        if not self.target.has_cap("ATOMIC_PAUSE"):
            util.SMlog("Deactivate & detach")
            self._deactivate(sr_uuid, vdi_uuid, {})
            self._detach(sr_uuid, vdi_uuid)
        else:
            pass # nothing to do
    
    def _deactivate(self, sr_uuid, vdi_uuid, caching_params):
        import VDI as sm

        # Shutdown tapdisk
        back_link = self.BackendLink.from_uuid(sr_uuid, vdi_uuid)
        if not util.pathexists(back_link.path()):
            util.SMlog("Backend path %s does not exist" % back_link.path())
            return

        try:
            attach_info_path = "%s.attach_info" % (back_link.path())
            os.unlink(attach_info_path)
        except:
            util.SMlog("unlink of attach_info failed")

        try:
            major, minor = back_link.rdev()
        except self.DeviceNode.NotABlockDevice:
            pass
        else:
            if major == Tapdisk.major():
                self._tap_deactivate(minor)
                self.remove_cache(sr_uuid, vdi_uuid, caching_params)

        # Remove the backend link
        back_link.unlink()

        # Deactivate & detach the physical node
        if self.tap_wanted():
            # it is possible that while the VDI was paused some of its 
            # attributes have changed (e.g. its size if it was inflated; or its 
            # path if it was leaf-coalesced onto a raw LV), so refresh the 
            # object completely
            target = sm.VDI.from_uuid(self.target.vdi.session, vdi_uuid)
            driver_info = target.sr.srcmd.driver_info
            self.target = self.TargetDriver(target, driver_info)

        self.target.deactivate(sr_uuid, vdi_uuid)

    def _detach(self, sr_uuid, vdi_uuid):
        self.target.detach(sr_uuid, vdi_uuid)

        # Remove phy/
        self.PhyLink.from_uuid(sr_uuid, vdi_uuid).unlink()

    def _updateCacheRecord(self, session, vdi_uuid, on_boot, caching):
        # Remove existing VDI.sm_config fields
        vdi_ref = session.xenapi.VDI.get_by_uuid(vdi_uuid)
        for key in ["on_boot", "caching"]:
            session.xenapi.VDI.remove_from_sm_config(vdi_ref,key)
        if not on_boot is None:
            session.xenapi.VDI.add_to_sm_config(vdi_ref,'on_boot',on_boot)
        if not caching is None:
            session.xenapi.VDI.add_to_sm_config(vdi_ref,'caching',caching)
	
    def setup_cache(self, sr_uuid, vdi_uuid, params):
        if params.get(self.CONF_KEY_ALLOW_CACHING) != "true":
            return

        util.SMlog("Requested local caching")
        if not self.target.has_cap("SR_CACHING"):
            util.SMlog("Error: local caching not supported by this SR")
            return

        scratch_mode = False
        if params.get(self.CONF_KEY_MODE_ON_BOOT) == "reset":
            scratch_mode = True
            util.SMlog("Requested scratch mode")
            if not self.target.has_cap("VDI_RESET_ON_BOOT/2"):
                util.SMlog("Error: scratch mode not supported by this SR")
                return

        session = XenAPI.xapi_local()
        session.xenapi.login_with_password('root', '')

        dev_path = None
        local_sr_uuid = params.get(self.CONF_KEY_CACHE_SR)
        if not local_sr_uuid:
            util.SMlog("ERROR: Local cache SR not specified, not enabling")
            return
        dev_path = self._setup_cache(session, sr_uuid, vdi_uuid,
                local_sr_uuid, scratch_mode, params)

        if dev_path:
            self._updateCacheRecord(session, self.target.vdi.uuid,
                    params.get(self.CONF_KEY_MODE_ON_BOOT),
                    params.get(self.CONF_KEY_ALLOW_CACHING))

        session.xenapi.session.logout()
        return dev_path

    def alert_no_cache(self, session, vdi_uuid, cache_sr_uuid, err):
        vm_uuid = None
        vm_label = ""
        try:
            cache_sr_ref = session.xenapi.SR.get_by_uuid(cache_sr_uuid)
            cache_sr_rec = session.xenapi.SR.get_record(cache_sr_ref)
            cache_sr_label = cache_sr_rec.get("name_label")

            host_ref = session.xenapi.host.get_by_uuid(util.get_this_host())
            host_rec = session.xenapi.host.get_record(host_ref)
            host_label = host_rec.get("name_label")

            vdi_ref = session.xenapi.VDI.get_by_uuid(vdi_uuid)
            vbds = session.xenapi.VBD.get_all_records_where( \
                    "field \"VDI\" = \"%s\"" % vdi_ref)
            for vbd_rec in vbds.values():
                vm_ref = vbd_rec.get("VM")
                vm_rec = session.xenapi.VM.get_record(vm_ref)
                vm_uuid = vm_rec.get("uuid")
                vm_label = vm_rec.get("name_label")
        except:
            util.logException("alert_no_cache")
        
        alert_obj = "SR"
        alert_uuid = str(cache_sr_uuid)
        alert_str = "No space left in Local Cache SR %s" % cache_sr_uuid
        if vm_uuid:
            alert_obj = "VM"
            alert_uuid = vm_uuid
            reason = ""
            if err == errno.ENOSPC:
                reason = "because there is no space left"
            alert_str = "The VM \"%s\" is not using IntelliCache %s on the Local Cache SR (\"%s\") on host \"%s\"" % \
                    (vm_label, reason, cache_sr_label, host_label)

        util.SMlog("Creating alert: (%s, %s, \"%s\")" % \
                (alert_obj, alert_uuid, alert_str))
        session.xenapi.message.create("No space left in local cache", "3",
                alert_obj, alert_uuid, alert_str)

    def _setup_cache(self, session, sr_uuid, vdi_uuid, local_sr_uuid,
            scratch_mode, options):
        import SR
        import EXTSR
        import NFSSR
        import XenAPI
        from lock import Lock
        from FileSR import FileVDI

        parent_uuid = vhdutil.getParent(self.target.vdi.path,
                FileVDI.extractUuid)
        if not parent_uuid:
            util.SMlog("ERROR: VDI %s has no parent, not enabling" % \
                    self.target.vdi.uuid)
            return

        util.SMlog("Setting up cache")
        parent_uuid = parent_uuid.strip()
        shared_target = NFSSR.NFSFileVDI(self.target.vdi.sr, parent_uuid)

        SR.registerSR(EXTSR.EXTSR)
        local_sr = SR.SR.from_uuid(session, local_sr_uuid)

        lock = Lock(self.LOCK_CACHE_SETUP, parent_uuid)
        lock.acquire()

        # read cache
        read_cache_path = "%s/%s.vhdcache" % (local_sr.path, shared_target.uuid)
        if util.pathexists(read_cache_path):
            util.SMlog("Read cache node (%s) already exists, not creating" % \
                    read_cache_path)
        else:
            try:
                vhdutil.snapshot(read_cache_path, shared_target.path, False)
            except util.CommandException, e:
                util.SMlog("Error creating parent cache: %s" % e)
                self.alert_no_cache(session, vdi_uuid, local_sr_uuid, e.code)
                return None

        # local write node
        leaf_size = vhdutil.getSizeVirt(self.target.vdi.path)
        local_leaf_path = "%s/%s.vhdcache" % \
                (local_sr.path, self.target.vdi.uuid)
        if util.pathexists(local_leaf_path):
            util.SMlog("Local leaf node (%s) already exists, deleting" % \
                    local_leaf_path)
            os.unlink(local_leaf_path)
        try:
            vhdutil.snapshot(local_leaf_path, read_cache_path, False,
                    msize = leaf_size / 1024 / 1024, checkEmpty = False)
        except util.CommandException, e:
            util.SMlog("Error creating leaf cache: %s" % e)
            self.alert_no_cache(session, vdi_uuid, local_sr_uuid, e.code)
            return None

        local_leaf_size = vhdutil.getSizeVirt(local_leaf_path)
        if leaf_size > local_leaf_size:
            util.SMlog("Leaf size %d > local leaf cache size %d, resizing" %
                    (leaf_size, local_leaf_size))
            vhdutil.setSizeVirtFast(local_leaf_path, leaf_size)

        vdi_type = self.target.get_vdi_type()

        prt_tapdisk = Tapdisk.find_by_path(read_cache_path)
        if not prt_tapdisk:
            parent_options = copy.deepcopy(options)
            parent_options["rdonly"] = False
            parent_options["lcache"] = True

            blktap = Blktap.allocate()
            try:
                blktap.set_pool_name("lcache-parent-pool-%s" % blktap.minor)
                # no need to change pool_size since each parent tapdisk is in 
                # its own pool
                prt_tapdisk = \
                    Tapdisk.launch_on_tap(blktap, read_cache_path,
                            'vhd', parent_options)
            except:
                blktap.free()
                raise

        secondary = "%s:%s" % (self.target.get_vdi_type(),
                self.PhyLink.from_uuid(sr_uuid, vdi_uuid).readlink())

        util.SMlog("Parent tapdisk: %s" % prt_tapdisk)
        leaf_tapdisk = Tapdisk.find_by_path(local_leaf_path)
        if not leaf_tapdisk:
            blktap = Blktap.allocate()
            child_options = copy.deepcopy(options)
            child_options["rdonly"] = False
            child_options["lcache"] = False
            child_options["existing_prt"] = prt_tapdisk.minor
            child_options["secondary"] = secondary
            child_options["standby"] = scratch_mode
            try:
                leaf_tapdisk = \
                    Tapdisk.launch_on_tap(blktap, local_leaf_path,
                            'vhd', child_options)
            except:
                blktap.free()
                raise

        lock.release()

        util.SMlog("Local read cache: %s, local leaf: %s" % \
                (read_cache_path, local_leaf_path))

        return leaf_tapdisk.get_devpath()

    def remove_cache(self, sr_uuid, vdi_uuid, params):
        if not self.target.has_cap("SR_CACHING"):
            return

        caching = params.get(self.CONF_KEY_ALLOW_CACHING) == "true"

        local_sr_uuid = params.get(self.CONF_KEY_CACHE_SR)
        if caching and not local_sr_uuid:
            util.SMlog("ERROR: Local cache SR not specified, ignore")
            return

        session = XenAPI.xapi_local()
        session.xenapi.login_with_password('root', '')

        if caching:
            self._remove_cache(session, local_sr_uuid)

        self._updateCacheRecord(session, self.target.vdi.uuid, None, None)
        session.xenapi.session.logout()

    def _is_tapdisk_in_use(self, minor):
        (retVal, links) = util.findRunningProcessOrOpenFile("tapdisk")
        if not retVal:
            # err on the side of caution
            return True
        
        for link in links:
            if link.find("tapdev%d" % minor) != -1:
                return True
        return False

    def _remove_cache(self, session, local_sr_uuid):
        import SR
        import EXTSR
        import NFSSR
        import XenAPI
        from lock import Lock
        from FileSR import FileVDI

        parent_uuid = vhdutil.getParent(self.target.vdi.path,
                FileVDI.extractUuid)
        if not parent_uuid:
            util.SMlog("ERROR: No parent for VDI %s, ignore" % \
                    self.target.vdi.uuid)
            return

        util.SMlog("Tearing down the cache")

        parent_uuid = parent_uuid.strip()
        shared_target = NFSSR.NFSFileVDI(self.target.vdi.sr, parent_uuid)

        SR.registerSR(EXTSR.EXTSR)
        local_sr = SR.SR.from_uuid(session, local_sr_uuid)

        lock = Lock(self.LOCK_CACHE_SETUP, parent_uuid)
        lock.acquire()

        # local write node
        local_leaf_path = "%s/%s.vhdcache" % \
                (local_sr.path, self.target.vdi.uuid)
        if util.pathexists(local_leaf_path):
            util.SMlog("Deleting local leaf node %s" % local_leaf_path)
            os.unlink(local_leaf_path)


        read_cache_path = "%s/%s.vhdcache" % (local_sr.path, shared_target.uuid)
        prt_tapdisk = Tapdisk.find_by_path(read_cache_path)
        if not prt_tapdisk:
            util.SMlog("Parent tapdisk not found")
        elif not self._is_tapdisk_in_use(prt_tapdisk.minor):
            util.SMlog("Parent tapdisk not in use: shutting down %s" % \
                    read_cache_path)
            try:
                prt_tapdisk.shutdown()
            except:
                util.logException("shutting down parent tapdisk")
        else:
            util.SMlog("Parent tapdisk still in use: %s" % read_cache_path)

        # the parent cache files are removed during the local SR's background 
        # GC run

        lock.release()


class UEventHandler(object):

    def __init__(self):
        self._action = None

    class KeyError(KeyError):
        def __str__(self):
            return \
                "Key '%s' missing in environment. " % self.args[0] + \
                "Not called in udev context?"

    @classmethod
    def getenv(cls, key):
        try:
            return os.environ[key]
        except KeyError, e:
            raise cls.KeyError(e.args[0])

    def get_action(self):
        if not self._action:
            self._action = self.getenv('ACTION')
        return self._action

    class UnhandledEvent(Exception):

        def __init__(self, event, handler):
            self.event   = event
            self.handler = handler

        def __str__(self):
            return "Uevent '%s' not handled by %s" % \
                (self.event, self.handler.__class__.__name__)

    ACTIONS = {}

    def run(self):

        action = self.get_action()
        try:
            fn = self.ACTIONS[action]
        except KeyError:
            raise self.UnhandledEvent(action, self)

        return fn(self)

    def __str__(self):
        try:    action = self.get_action()
        except: action = None
        return "%s[%s]" % (self.__class__.__name__, action)

class __BlktapControl(ClassDevice):
    SYSFS_CLASSTYPE = "misc"

    def __init__(self):
        ClassDevice.__init__(self)
        self._default_pool = None

    def sysfs_devname(self):
        return "blktap-control"

    class DefaultPool(Attribute):
        SYSFS_NODENAME = "default_pool"

    def get_default_pool_attr(self):
        if not self._default_pool:
            self._default_pool = self.DefaultPool.from_kobject(self)
        return self._default_pool

    def get_default_pool_name(self):
        return self.get_default_pool_attr().readline()

    def set_default_pool_name(self, name):
        self.get_default_pool_attr().writeline(name)

    def get_default_pool(self):
        return BlktapControl.get_pool(self.get_default_pool_name())

    def set_default_pool(self, pool):
        self.set_default_pool_name(pool.name)

    class NoSuchPool(Exception):
        def __init__(self, name):
            self.name = name

        def __str__(self):
            return "No such pool: %s", self.name

    def get_pool(self, name):
        path = "%s/pools/%s" % (self.sysfs_path(), name)

        if not os.path.isdir(path):
            raise self.NoSuchPool(name)

        return PagePool(path)

BlktapControl = __BlktapControl()

class PagePool(KObject):
    
    def __init__(self, path):
        self.path = path
        self._size = None

    def sysfs_path(self):
        return self.path

    class Size(Attribute):
        SYSFS_NODENAME = "size"

    def get_size_attr(self):
        if not self._size:
            self._size = self.Size.from_kobject(self)
        return self._size

    def set_size(self, pages):
        pages = str(pages)
        self.get_size_attr().writeline(pages)

    def get_size(self):
        pages = self.get_size_attr().readline()
        return int(pages)

class BusDevice(KObject):

    SYSFS_BUSTYPE = None

    @classmethod
    def sysfs_bus_path(cls):
        return "/sys/bus/%s" % cls.SYSFS_BUSTYPE

    def sysfs_path(self):
        path = "%s/devices/%s" % (self.sysfs_bus_path(),
                                  self.sysfs_devname())
        
        return path

class XenbusDevice(BusDevice):
    """Xenbus device, in XS and sysfs"""

    XBT_NIL = ""

    XENBUS_DEVTYPE = None

    def __init__(self, domid, devid):
        self.domid = int(domid)
        self.devid = int(devid)
        self._xbt  = XenbusDevice.XBT_NIL

        import xen.lowlevel.xs
        self.xs = xen.lowlevel.xs.xs()

    def xs_path(self, key=None):
        path = "backend/%s/%d/%d" % (self.XENBUS_DEVTYPE,
                                     self.domid,
                                     self.devid)
        if key is not None:
            path = "%s/%s" % (path, key)

        return path

    def _log(self, prio, msg):
        syslog(prio, msg)

    def info(self, msg):
        self._log(_syslog.LOG_INFO, msg)

    def warn(self, msg):
        self._log(_syslog.LOG_WARNING, "WARNING: " + msg)

    def _xs_read_path(self, path):
        val = self.xs.read(self._xbt, path)
        #self.info("read %s = '%s'" % (path, val))
        return val

    def _xs_write_path(self, path, val):
        self.xs.write(self._xbt, path, val);
        self.info("wrote %s = '%s'" % (path, val))

    def _xs_rm_path(self, path):
        self.xs.rm(self._xbt, path)
        self.info("removed %s" % path)

    def read(self, key):
        return self._xs_read_path(self.xs_path(key))

    def has_key(self, key):
        return self.read(key) is not None

    def write(self, key, val):
        self._xs_write_path(self.xs_path(key), val)

    def rm(self, key):
        self._xs_rm_path(self.xs_path(key))

    def exists(self):
        return self.has_key(None)

    def begin(self):
        assert(self._xbt == XenbusDevice.XBT_NIL)
        self._xbt = self.xs.transaction_start()

    def commit(self):
        ok = self.xs.transaction_end(self._xbt, 0)
        self._xbt = XenbusDevice.XBT_NIL
        return ok

    def abort(self):
        ok = self.xs.transaction_end(self._xbt, 1)
        assert(ok == True)
        self._xbt = XenbusDevice.XBT_NIL

    def create_physical_device(self):
        """The standard protocol is: toolstack writes 'params', linux hotplug
        script translates this into physical-device=%x:%x"""
        if self.has_key("physical-device"):
            return
        try:
            params = self.read("params")
            frontend = self.read("frontend")
            is_cdrom = self._xs_read_path("%s/device-type") == "cdrom"
            # We don't have PV drivers for CDROM devices, so we prevent blkback
            # from opening the physical-device
            if not(is_cdrom):
                major_minor = os.stat(params).st_rdev
                major, minor = divmod(major_minor, 256)
                self.write("physical-device", "%x:%x" % (major, minor))
        except:
            util.logException("BLKTAP2:create_physical_device")

    def signal_hotplug(self, online=True):
        xapi_path = "/xapi/%d/hotplug/%s/%d/hotplug" % (self.domid, 
                                                   self.XENBUS_DEVTYPE,
                                                   self.devid)
        upstream_path = self.xs_path("hotplug-status")
        if online:
            self._xs_write_path(xapi_path, "online")
            self._xs_write_path(upstream_path, "connected")
        else:
            self._xs_rm_path(xapi_path)
            self._xs_rm_path(upstream_path)

    def sysfs_devname(self):
        return "%s-%d-%d" % (self.XENBUS_DEVTYPE, 
                             self.domid, self.devid)

    def __str__(self):
        return self.sysfs_devname()

    @classmethod
    def find(cls):
        pattern = "/sys/bus/%s/devices/%s*" % (cls.SYSFS_BUSTYPE,
                                               cls.XENBUS_DEVTYPE)
        for path in glob.glob(pattern):

            name = os.path.basename(path)
            (_type, domid, devid) = name.split('-')

            yield cls(domid, devid)

class XenBackendDevice(XenbusDevice):
    """Xenbus backend device"""
    SYSFS_BUSTYPE = "xen-backend"

    @classmethod
    def from_xs_path(cls, _path):
        (_backend, _type, domid, devid) = _path.split('/')

        assert _backend == 'backend'
        assert _type == cls.XENBUS_DEVTYPE

        domid = int(domid)
        devid = int(devid)

        return cls(domid, devid)

class Blkback(XenBackendDevice):
    """A blkback VBD"""

    XENBUS_DEVTYPE = "vbd"

    def __init__(self, domid, devid):
        XenBackendDevice.__init__(self, domid, devid)
        self._phy      = None
        self._vdi_uuid = None
        self._q_state  = None
        self._q_events = None

    class XenstoreValueError(Exception):
        KEY = None
        def __init__(self, vbd, _str):
            self.vbd = vbd
            self.str = _str

        def __str__(self):
            return "Backend %s " % self.vbd + \
                "has %s = %s" % (self.KEY, self.str)

    class PhysicalDeviceError(XenstoreValueError):
        KEY = "physical-device"

    class PhysicalDevice(object):

        def __init__(self, major, minor):
            self.major = int(major)
            self.minor = int(minor)

        @classmethod
        def from_xbdev(cls, xbdev):

            phy = xbdev.read("physical-device")

            try:
                major, minor = phy.split(':')
                major = int(major, 0x10)
                minor = int(minor, 0x10)
            except Exception, e:
                raise xbdev.PhysicalDeviceError(xbdev, phy)

            return cls(major, minor)

        def makedev(self):
            return os.makedev(self.major, self.minor)

        def is_tap(self):
            return self.major == Tapdisk.major()

        def __str__(self):
            return "%s:%s" % (self.major, self.minor)

        def __eq__(self, other):
            return \
                self.major == other.major and \
                self.minor == other.minor

    def get_physical_device(self):
        if not self._phy:
            self._phy = self.PhysicalDevice.from_xbdev(self)
        return self._phy

    class QueueEvents(Attribute):
        """Blkback sysfs node to select queue-state event
        notifications emitted."""

        SYSFS_NODENAME = "queue_events"

        QUEUE_RUNNING           = (1<<0)
        QUEUE_PAUSE_DONE        = (1<<1)
        QUEUE_SHUTDOWN_DONE     = (1<<2)
        QUEUE_PAUSE_REQUEST     = (1<<3)
        QUEUE_SHUTDOWN_REQUEST  = (1<<4)

        def get_mask(self):
            return int(self.readline(), 0x10)

        def set_mask(self, mask):
            self.writeline("0x%x" % mask)

    def get_queue_events(self):
        if not self._q_events:
            self._q_events = self.QueueEvents.from_kobject(self)
        return self._q_events

    def get_vdi_uuid(self):
        if not self._vdi_uuid:
            self._vdi_uuid = self.read("sm-data/vdi-uuid")
        return self._vdi_uuid

    def pause_requested(self):
        return self.has_key("pause")

    def shutdown_requested(self):
        return self.has_key("shutdown-request")

    def shutdown_done(self):
        return self.has_key("shutdown-done")

    def kthread_pid(self):
        pid = self.read("kthread-pid")
        if pid is not None: return int(pid)
        return None

    def running(self):
        return self.kthread_pid() is not None

    @classmethod
    def find_by_physical_device(cls, phy):
        for dev in cls.find():
            try:
                _phy = dev.get_physical_device()
            except cls.PhysicalDeviceError:
                continue

            if _phy == phy:
                yield dev

    @classmethod
    def find_by_tap_minor(cls, minor):
        phy = cls.PhysicalDevice(Tapdisk.major(), minor)
        return cls.find_by_physical_device(phy)

    @classmethod
    def find_by_tap(cls, tapdisk):
        return cls.find_by_tap_minor(tapdisk.minor)

    def has_tap(self):

        if not self.can_tap():
            return False

        phy = self.get_physical_device()
        if phy:
            return phy.is_tap()

        return False

    def is_bare_hvm(self):
        """File VDIs for bare HVM. These are directly accessible by Qemu."""
        try:
            self.get_physical_device()

        except self.PhysicalDeviceError, e:
            vdi_type = self.read("type")

            self.info("HVM VDI: type=%s" % vdi_type)

            if e.str is not None or vdi_type != 'file':
                raise

            return True

        return False

    def can_tap(self):
        return not self.is_bare_hvm()


class BlkbackEventHandler(UEventHandler):

    LOG_FACILITY = _syslog.LOG_DAEMON

    def __init__(self, ident=None, action=None):
        if not ident: ident = self.__class__.__name__

        self.ident    = ident
        self._vbd     = None
        self._tapdisk = None

        UEventHandler.__init__(self)

    def run(self):

        self.xs_path = self.getenv('XENBUS_PATH')
        openlog(str(self), 0, self.LOG_FACILITY)

        UEventHandler.run(self)

    def __str__(self):

        try:    path = self.xs_path
        except: path = None

        try:    action = self.get_action()
        except: action = None

        return "%s[%s](%s)" % (self.ident, action, path)

    def _log(self, prio, msg):
        syslog(prio, msg)
        util.SMlog("%s: " % self + msg)

    def info(self, msg):
        self._log(_syslog.LOG_INFO, msg)

    def warn(self, msg):
        self._log(_syslog.LOG_WARNING, "WARNING: " + msg)

    def error(self, msg):
        self._log(_syslog.LOG_ERR, "ERROR: " + msg)

    def get_vbd(self):
        if not self._vbd:
            self._vbd = Blkback.from_xs_path(self.xs_path)
        return self._vbd

    def get_tapdisk(self):
        if not self._tapdisk:
            minor = self.get_vbd().get_physical_device().minor
            self._tapdisk = Tapdisk.from_minor(minor)
        return self._tapdisk

    #
    # Events
    #

    def __add(self):
        vbd = self.get_vbd()

        # Manage blkback transitions
        # self._manage_vbd()

        vbd.create_physical_device()

        vbd.signal_hotplug()

    @retried(backoff=.5, limit=10)
    def add(self):
        try:
            self.__add()
        except Attribute.NoSuchAttribute, e:
            #
            # FIXME: KOBJ_ADD is racing backend.probe, which
            # registers device attributes. So poll a little.
            #
            self.warn("%s, still trying." % e)
            raise RetryLoop.TransientFailure(e)

    def __change(self):
        vbd = self.get_vbd()

        # 1. Pause or resume tapdisk (if there is one)

        if vbd.has_tap():
            pass
            #self._pause_update_tap()

        # 2. Signal Xapi.VBD.pause/resume completion

        self._signal_xapi()

    def change(self):
        vbd = self.get_vbd()

        # NB. Beware of spurious change events between shutdown
        # completion and device removal. Also, Xapi.VM.migrate will
        # hammer a couple extra shutdown-requests into the source VBD.

        while True:
            vbd.begin()

            if not vbd.exists() or \
                    vbd.shutdown_done():
                break

            self.__change()

            if vbd.commit():
                return

        vbd.abort()
        self.info("spurious uevent, ignored.")

    def remove(self):
        vbd = self.get_vbd()

        vbd.signal_hotplug(False)

    ACTIONS = { 'add':    add,
                'change': change,
                'remove': remove }

    #
    # VDI.pause
    #

    def _tap_should_pause(self):
        """Enumerate all VBDs on our tapdisk. Returns true iff any was
        paused"""

        tapdisk  = self.get_tapdisk()
        TapState = Tapdisk.PauseState

        PAUSED          = 'P'
        RUNNING         = 'R'
        PAUSED_SHUTDOWN = 'P,S'
        # NB. Shutdown/paused is special. We know it's not going
        # to restart again, so it's a RUNNING. Still better than
        # backtracking a removed device during Vbd.unplug completion.

        next = TapState.RUNNING
        vbds = {}

        for vbd in Blkback.find_by_tap(tapdisk):
            name = str(vbd)

            pausing = vbd.pause_requested()
            closing = vbd.shutdown_requested()
            running = vbd.running()

            if pausing:
                if closing and not running:
                    vbds[name] = PAUSED_SHUTDOWN
                else:
                    vbds[name] = PAUSED
                    next = TapState.PAUSED

            else:
                vbds[name] = RUNNING

        self.info("tapdev%d (%s): %s -> %s" 
                  % (tapdisk.minor, tapdisk.pause_state(),
                     vbds, next))

        return next == TapState.PAUSED

    def _pause_update_tap(self):
        vbd = self.get_vbd()

        if self._tap_should_pause():
            self._pause_tap()
        else:
            self._resume_tap()

    def _pause_tap(self):
        tapdisk = self.get_tapdisk()

        if not tapdisk.is_paused():
            self.info("pausing %s" % tapdisk)
            tapdisk.pause()

    def _resume_tap(self):
        tapdisk = self.get_tapdisk()

        # NB. Raw VDI snapshots. Refresh the physical path and
        # type while resuming.
        vbd      = self.get_vbd()
        vdi_uuid = vbd.get_vdi_uuid()

        if tapdisk.is_paused():
            self.info("loading vdi uuid=%s" % vdi_uuid)
            vdi      = VDI.from_cli(vdi_uuid)
            _type    = vdi.get_tap_type()
            path     = vdi.get_phy_path()
            self.info("resuming %s on %s:%s" % (tapdisk, _type, path))
            tapdisk.unpause(_type, path)

    #
    # VBD.pause/shutdown
    #

    def _manage_vbd(self):
        vbd = self.get_vbd()

        # NB. Hook into VBD state transitions.

        events = vbd.get_queue_events()

        mask  = 0
        mask |= events.QUEUE_PAUSE_DONE    # pause/unpause
        mask |= events.QUEUE_SHUTDOWN_DONE # shutdown

        # TODO: mask |= events.QUEUE_SHUTDOWN_REQUEST, for shutdown=force
        # TODO: mask |= events.QUEUE_RUNNING, for ionice updates etc

        events.set_mask(mask)
        self.info("wrote %s = %#02x" % (events.path, mask))

    def _signal_xapi(self):
        vbd = self.get_vbd()

        pausing = vbd.pause_requested()
        closing = vbd.shutdown_requested()
        running = vbd.running()

        handled = 0

        if pausing and not running:
            if not vbd.has_key('pause-done'):
                vbd.write('pause-done', '')
                handled += 1

        if not pausing:
            if vbd.has_key('pause-done'):
                vbd.rm('pause-done')
                handled += 1

        if closing and not running:
            if not vbd.has_key('shutdown-done'):
                vbd.write('shutdown-done', '')
                handled += 1

        if handled > 1:
            self.warn("handled %d events, " % handled +
                      "pausing=%s closing=%s running=%s" % \
                          (pausing, closing, running))

if __name__ == '__main__':

    import sys
    prog  = os.path.basename(sys.argv[0])

    #
    # Simple CLI interface for manual operation
    #
    #  tap.*  level calls go down to local Tapdisk()s (by physical path)
    #  vdi.*  level calls run the plugin calls across host boundaries.
    #

    def usage(stream):
        print >>stream, \
            "usage: %s tap.{list|major}" % prog
        print >>stream, \
            "       %s tap.{launch|find|get|pause|" % prog + \
            "unpause|shutdown|stats} {[<tt>:]<path>} | [minor=]<int> | .. }"
        print >>stream, \
            "       %s vbd.uevent" % prog

    try:
        cmd = sys.argv[1]
    except IndexError:
        usage(sys.stderr)
        sys.exit(1)

    try:
        _class, method = cmd.split('.')
    except:
        usage(sys.stderr)
        sys.exit(1)

    #
    # Local Tapdisks
    #

    if cmd == 'tap.major':

        print "%d" % Tapdisk.major()

    elif cmd == 'tap.launch':

        tapdisk = Tapdisk.launch_from_arg(sys.argv[2])
        print >> sys.stderr, "Launched %s" % tapdisk

    elif _class == 'tap':

        attrs = {}
        for item in sys.argv[2:]:
            try:
                key, val = item.split('=')
                attrs[key] = val
                continue
            except ValueError:
                pass

            try:
                attrs['minor'] = int(item)
                continue
            except ValueError:
                pass

            try:
                arg = Tapdisk.Arg.parse(item)
                attrs['_type'] = arg.type
                attrs['path']  = arg.path
                continue
            except Tapdisk.Arg.InvalidArgument:
                pass

            attrs['path'] = item

        if cmd == 'tap.list':

            for tapdisk in Tapdisk.list(**attrs):
                blktap = tapdisk.get_blktap()
                print tapdisk,
                print "%s: task=%s pool=%s" % \
                    (blktap,
                     blktap.get_task_pid(),
                     blktap.get_pool_name())

        elif cmd == 'tap.vbds':
            # Find all Blkback instances for a given tapdisk

            for tapdisk in Tapdisk.list(**attrs):
                print "%s:" % tapdisk,
                for vbd in Blkback.find_by_tap(tapdisk): 
                    print vbd,
                print

        else:

            if not attrs:
                usage(sys.stderr)
                sys.exit(1)

            try:
                tapdisk = Tapdisk.get(**attrs)
            except TypeError:
                usage(sys.stderr)
                sys.exit(1)

            if cmd == 'tap.shutdown':
                # Shutdown a running tapdisk, or raise
                tapdisk.shutdown()
                print >> sys.stderr, "Shut down %s" % tapdisk

            elif cmd == 'tap.pause':
                # Pause an unpaused tapdisk, or raise
                tapdisk.pause()
                print >> sys.stderr, "Paused %s" % tapdisk

            elif cmd == 'tap.unpause':
                # Unpause a paused tapdisk, or raise
                tapdisk.unpause()
                print >> sys.stderr, "Unpaused %s" % tapdisk

            elif cmd == 'tap.stats':
                # Gather tapdisk status
                stats = tapdisk.stats()
                print "%s:" % tapdisk
                print json.dumps(stats, indent=True)

            else:
                usage(sys.stderr)
                sys.exit(1)

    elif cmd == 'vbd.uevent':

        hnd = BlkbackEventHandler(cmd)

        if not sys.stdin.isatty():
            try:
                hnd.run()
            except Exception, e:
                hnd.error("Unhandled Exception: %s" % e)

                import traceback
                _type, value, tb = sys.exc_info()
                trace = traceback.format_exception(_type, value, tb)
                for entry in trace:
                    for line in entry.rstrip().split('\n'):
                        util.SMlog(line)
        else:
            hnd.run()

    elif cmd == 'vbd.list':

        for vbd in Blkback.find(): 
            print vbd, \
                "physical-device=%s" % vbd.get_physical_device(), \
                "pause=%s" % vbd.pause_requested()

    else:
        usage(sys.stderr)
        sys.exit(1)
