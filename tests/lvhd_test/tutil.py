# Various utility functions.
# TODO why the leading "t" in the filename?

import time
import os
import datetime
import subprocess
import pexpect
import re
import sys
import tempfile

KiB = 1024
MiB = KiB * KiB
GiB = KiB * MiB
TiB = KiB * GiB

UUID_LEN = 36

RET_RC     = 1
RET_STDERR = 2
RET_STDOUT = 4


class CommandException(Exception):
    pass

class Logger:
    "Log output to a file"

    def __init__(self, fn, verbosity):
        """Constructor. fn: file to log. verbosity: an integer value that is
        compared with the verbosity argument of the log method. If the latter
        is found greater than the former, the message is not logged."""

        self._logfile = open(fn, 'a')
        self._verbosity = verbosity
        self.log("===== Log started, verbosity=%d, pid=%s =====" % \
                (verbosity, os.getpid()), 1)

    def log(self, msg, verbosity = 1):
        """Logs the message 'msg'. If the specified verbosity is higher than
        the one used at object-construction time, the message is not logged.
        If verbosity is zero, the message is printed on the screen."""

        if verbosity > self._verbosity:
            return
        indent = "  " * (verbosity - 1)
        self._logfile.write("%s  %s%s\n" % \
                (datetime.datetime.now(), indent, msg))
        self._logfile.flush()
        if verbosity == 0:
            print msg

    def write(self, msg):
        self._logfile.write("%s\n" % msg)
        self._logfile.flush()

def str2int(strNum):
    num = 0
    if strNum[-1] in ['K', 'k']:
        num = int(strNum[:-1]) * KiB
    elif strNum[-1] in ['M', 'm']:
        num = int(strNum[:-1]) * MiB
    elif strNum[-1] in ['G', 'g']:
        num = int(strNum[:-1]) * GiB
    elif strNum[-1] in ['T', 't']:
        num = int(strNum[:-1]) * TiB
    else:
        num = int(strNum)
    return num

# TODO rename to _exec
def doexec(args, inputtext=None):
        "Execute a subprocess, then return its return code, stdout, stderr"
        proc = subprocess.Popen(args,
                                stdin=subprocess.PIPE,\
                                stdout=subprocess.PIPE,\
                                stderr=subprocess.PIPE,\
                                shell=True,\
                                close_fds=True)
        (stdout,stderr) = proc.communicate(inputtext)
        rc = proc.returncode
        return (rc,stdout,stderr)

# TODO rename to exec
def execCmd(cmd, expectedRC, logger, verbosity = 1, ret = None):
    logger.log("`%s`" % cmd, verbosity)
    (rc, stdout, stderr) = doexec(cmd)
    stdoutSnippet = stdout.split('\n')[0]
    if stdoutSnippet != stdout.strip():
        stdoutSnippet += " ..."
    stderrSnippet = stderr.split('\n')[0]
    if stderrSnippet != stderr.strip():
        stderrSnippet += " ..."
    logger.log("(%d), \"%s\" (\"%s\")" % (rc, stdoutSnippet, stderrSnippet), \
            verbosity)
    if type(expectedRC) != type([]):
        expectedRC = [expectedRC]
    if not rc in expectedRC:
        raise CommandException("Command failed: '%s': %d != %s: %s (%s)" % \
                (cmd, rc, expectedRC, stdout.strip(), stderr.strip()))
    if ret == RET_RC:
        return rc
    if ret == RET_STDERR:
        return stderr
    return stdout

# TODO replace with os.path.exists
def pathExists(path):
    try:
        os.stat(path)
        return True
    except OSError:
        return False

# Regex that matches a UUID.
uuidre = '[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}'

def validateUUID(uuid):
    return None != re.match(uuidre, uuid)

def ssh(host, cmd, user = 'root', password = 'xenroot', timeout = 30):
    """SSH'es to a host using the supplied credentials and executes a command.
    Throws an exception if the command doesn't return 0."""

    fname = tempfile.mktemp()
    fout = open(fname, 'w')

    options = '-q -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null \
            -oPubkeyAuthentication=no'
    ssh_cmd = 'ssh ' + user + '@' + host + ' ' + options + ' "' + cmd + '"'
    child = pexpect.spawn(ssh_cmd, timeout = timeout)
    child.logfile = fout
    i = child.expect(['password: '])
    assert 0 == i # FIXME
    child.sendline(password)
    child.expect(pexpect.EOF)
    child.close()
    fout.close()

    if 0 != child.exitstatus:
        # FIXME read entire file in memory
        fin = open(fname, 'r')
        stdout = fin.read()
        fin.close()
        raise Exception('error executing command \'' + ssh_cmd + '\', return \
                code ' + str(child.exitstatus) + ', output ' + stdout)

def scp(host, src, dst = '', user = 'root', password = 'xenroot'):
    """Copies a file in a host. Throws an exception if the command doesn't return 0."""

    fname = tempfile.mktemp()
    fout = open(fname, 'w')
    if '' == dst:
        dst = src
    options = '-q -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null'
    child = pexpect.spawn('scp ' + options + ' "' + src + '" ' + user + '@' \
            + host + ':' + dst)
    child.logfile = fout
    i = child.expect(['password: '])
    assert 0 == i # FIXME
    child.sendline(password)
    child.expect(pexpect.EOF)
    child.close()
    fout.close()

    if 0 != child.exitstatus:
        # FIXME read entire file in memory
        fin = open(fname, 'r')
        stdout = fin.read
        fin.close()
        raise Exception('error copying, return code ' + str(child.exitstatus) \
                + ', output ' + stdout)

# XXX Not absolutely correct, probably matches 999.999.999.999.
ip_regex = '(?:[\d]{1,3})\.(?:[\d]{1,3})\.(?:[\d]{1,3})\.(?:[\d]{1,3})'
def vm_get_ip(vm_uuid):
    """Retrieves the IP address of a VM."""

    cmd = 'xe vm-list params=networks uuid=' + vm_uuid + ' --minimal';
    (rc, stdout, stderr) = doexec(cmd)

    if 0 != rc:
        print stdout
        assert False # FIXME
    
    mo = re.search(ip_regex, stdout)
    if not mo:
        print stdout
        return None

    return mo.group()
