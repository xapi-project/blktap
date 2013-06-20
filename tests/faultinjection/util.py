# Copyright (c) 2005-2007 XenSource, Inc. All use and distribution of this
# copyrighted material is governed by and subject to terms and conditions
# as licensed by XenSource, Inc. All other rights reserved.
# Xen, XenSource and XenEnterprise are either registered trademarks or
# trademarks of XenSource Inc. in the United States and/or other countries.
#
#
# Miscellaneous utility functions
#

import os, re
import time, datetime
import errno
import xml.dom.minidom
import SR
import statvfs
import stat
import signal, syslog

IORETRY_MAX = 200
IOSLEEP = 0.1
LOGGING = True
LOGFILE = '/var/log/SMlog'
STDERRLOG = LOGFILE

class CommandException(Exception):
    def __init__(self, code, reason='exec failed'):
        self.code = code
        Exception.__init__(self, code)

def shellquote(arg):
    return '"%s"' % arg.replace('"', '\\"')

def SMlog(str):
    if LOGGING:
        f=open(LOGFILE, 'a')
        f.write("%s\t%s\n" % (datetime.datetime.now(),str))
        f.close()

#Only read STDOUT from cmdlist
def pread(cmdlist):
    cmd = ' '.join([shellquote(arg) for arg in cmdlist])
    SMlog(cmd)
    fd = os.popen(cmd)
    out = ''
    while 1:
        val = fd.read()
        if not val:
            break
        out += val
    rc = fd.close()

    if rc:
        SMlog("FAILED: cmd [%s] (errno %d)" % (cmd,os.WEXITSTATUS(rc)))
        raise CommandException(os.WEXITSTATUS(rc))
    SMlog("SUCCESS: cmd [%s]" % cmd)

    return out

#Read STDOUT from cmdlist and discard STDERR output
def pread2(cmdlist):
    cmd = ' '.join([shellquote(arg) for arg in cmdlist])
    SMlog(cmd)
    fd = os.popen("%s 2>>%s" % (cmd,STDERRLOG))
    out = ''
    while 1:
        val = fd.read()
        if not val:
            break
        out += val
    rc = fd.close()

    if rc:
        SMlog("FAILED: cmd [%s] (errno %d)" % (cmd,os.WEXITSTATUS(rc)))
        raise CommandException(os.WEXITSTATUS(rc))
    SMlog("SUCCESS: cmd [%s]" % cmd)

    return out

#Write STDOUT to a file and discard STDERR output
def pread3(cmdlist, log):
    cmd = ' '.join([shellquote(arg) for arg in cmdlist])
    SMlog(cmd)
    fd = os.popen("%s 1>%s 2>>%s" % (cmd,log,STDERRLOG))
    rc = fd.close()
    if rc:
        SMlog("FAILED: cmd [%s] (errno %d)" % (cmd,os.WEXITSTATUS(rc)))
        raise CommandException(os.WEXITSTATUS(rc))
    SMlog("SUCCESS: cmd [%s]" % cmd)

def listdir(path):
    cmd = ["ls", path, "-1", "--color=never"]
    try:
        text = pread2(cmd).split('\n')
    except CommandException, inst:
        if inst.code == errno.ENOENT:
            raise CommandException(errno.EIO)
        else:
            raise CommandException(inst.code)
    return text

def gen_uuid():
    cmd = ["uuidgen", "-r"]
    return pread(cmd)[:-1]

def match_uuid(s):
    regex = re.compile("^[0-9a-f]{8}-(([0-9a-f]{4})-){3}[0-9a-f]{12}")
    return regex.search(s, 0)

def start_log_entry(srpath, path, args):
    logstring = str(datetime.datetime.now())
    logstring += " log: "
    logstring += srpath 
    logstring +=  " " + path
    for element in args:
        logstring += " " + element
    try:
        file = open(srpath + "/filelog.txt", "a")
        file.write(logstring)
        file.write("\n")
        file.close()
    except:
        pass
        # failed to write log ... 

def end_log_entry(srpath, path, args):
    # for teminating, use "error" or "done"
    logstring = str(datetime.datetime.now())
    logstring += " end: "
    logstring += srpath 
    logstring +=  " " + path
    for element in args:
        logstring += " " + element
    try:
        file = open(srpath + "/filelog.txt", "a")
        file.write(logstring)
        file.write("\n")
        file.close()
    except:
        pass
        # failed to write log ... 
    # for now print
    # print "%s" % logstring

def find_pfilter_pid():
    pid = 0
    cmd = ["ps","aux"]
    out = pread(cmd).split("\n")
    for findme in out:
        if findme.find('/usr/sbin/pfilter') != -1:
            splits = findme.split(" ")
            for findnum in splits:
                try:
                    pid = int(findnum)
                    syslog.syslog("FITrace: pfilter PID is %d" % pid)
                    break
                except:
                    pass
    return pid

def ioretry(f, errlist=[errno.EIO], maxretry=IORETRY_MAX, \
            nofail=0, iosleep=IOSLEEP):
    retries = 0
    pid = find_pfilter_pid()
    if pid != 0 and nofail:
        os.kill(pid, signal.SIGCHLD)
        syslog.syslog("FITrace: ioretry reset: ALLOW all packets (nofail)")
        time.sleep(iosleep)
    # reset 0 packets allowed
    if pid != 0 and not nofail:
        os.kill(pid, signal.SIGUSR1)
        syslog.syslog("FITrace: ioretry reset: BLOCK all packets")
        time.sleep(iosleep)
    while retries < maxretry:
        raiseme = 1
        try:
            value = f()
            # reset to allow all packets
            if pid != 0 and not nofail:
                syslog.syslog("FITrace: ioretry succeed: retry=%d" % retries)
                os.kill(pid, signal.SIGCHLD)
                syslog.syslog("FITrace: ioretry reset: ALLOW all packets")
                time.sleep(iosleep)
            return value
        except OSError, inst:
            for etest in errlist:
                if int(inst.errno) == etest:
                    retries += 1
                    raiseme = 0
                    # increment allowed packet count
                    if pid != 0 and not nofail:
                        os.kill(pid, signal.SIGUSR2)
                        syslog.syslog( \
                      "FITrace: ioretry adjust: INCREMENT accepted packets=%d" \
                      % retries)
                    time.sleep(iosleep)
                
            if raiseme:
                if pid != 0 and not nofail:
                    syslog.syslog( \
                             "FITrace: ioretry failed: retry=%d, errno=%d" \
                             % (retries, inst.errno))
                    os.kill(pid, signal.SIGCHLD)
                    syslog.syslog("FITrace: ioretry reset: ALLOW all packets")
                    time.sleep(iosleep)
                raise CommandException(inst.errno)
        except CommandException, inst:
            for etest in errlist:
                if int(inst.code) == etest:
                    retries += 1
                    raiseme = 0
                    # increment allowed packet count
                    if pid != 0 and not nofail:
                        os.kill(pid, signal.SIGUSR2)
                        syslog.syslog( \
                       "FITrace: ioretry adjust: INCREMENT accepted packets=%d" \
                        % retries)
                    time.sleep(iosleep)
            if raiseme:
                if pid != 0 and not nofail:
                    syslog.syslog( \
                              "FITrace: ioretry failed: retry=%d, errno=%d" \
                              % (retries, inst.code))
                    os.kill(pid, signal.SIGCHLD)
                    syslog.syslog("FITrace: ioretry reset: ALLOW all packets")
                    time.sleep(iosleep)
                raise CommandException(inst.code)
    SMlog("ioretry failed after %d packets dropped" % retries)
    if pid != 0:
        os.kill(pid, signal.SIGCHLD)
        syslog.syslog("FITrace: ioretry reset: ALLOW all packets")
        time.sleep(iosleep)
    raise CommandException(errno.EIO)

def ioretry_stat(f, maxretry=IORETRY_MAX, nofail=0):
    # this ioretry is similar to the previous method, but
    # stat does not raise an error -- so check its return
    retries = 0
    pid = find_pfilter_pid()
    # reset 0 packets allowed
    if pid != 0 and not nofail:
        os.kill(pid, signal.SIGUSR1)
        syslog.syslog("FITrace: ioretry reset: BLOCK all packets")
        time.sleep(1)
    while retries < maxretry:
        stat = f()
        if stat[statvfs.F_BLOCKS] != -1:
            return stat
        syslog.syslog("wkc: statvfs returned -1, retry=%d" % retries)
        if pid != 0 and not nofail:
            os.kill(pid, signal.SIGUSR2)
            syslog.syslog(
                 "FITrace: ioretry adjust: INCREMENT accepted packets=%d" \
                  % retries)
        time.sleep(1)
        retries += 1
    raise CommandException(errno.EIO)

def addCapabilitiesTo(dom, element, capabilities):
    for cap in capabilities:
        cap_elem = dom.createElement("vdi-capability")
        element.appendChild(cap_elem)
        textnode = dom.createTextNode(cap)
        cap_elem.appendChild(textnode)

def capabilityXML(capabilities):
    dom = xml.dom.minidom.Document()
    element = dom.createElement("sr")
    dom.appendChild(element)

    addCapabilitiesTo(dom, element, capabilities)

    return dom.toprettyxml()

def driverInfoXML(driver_info):
    dom = xml.dom.minidom.Document()
    driver = dom.createElement("driver")
    dom.appendChild(driver)

    # first add in the vanilla stuff
    for key in [ 'name', 'description', 'vendor', 'copyright', \
                 'driver_version', 'required_api_version' ]:
        e = dom.createElement(key)
        driver.appendChild(e)
        textnode = dom.createTextNode(driver_info[key])
        e.appendChild(textnode)
        
    cap = dom.createElement("capabilities")
    driver.appendChild(cap)
    addCapabilitiesTo(dom, cap, driver_info['capabilities'])

    def addConfigTo(dom, element, options):
        for option in options:
            o = dom.createElement("option")
            element.appendChild(o)
            e = dom.createElement("key")
            o.appendChild(e)
            text = dom.createTextNode(option[0])
            e.appendChild(text)
            e = dom.createElement("description")
            o.appendChild(e)
            text = dom.createTextNode(option[1])
            e.appendChild(text)

    config = dom.createElement("configuration")
    driver.appendChild(config)
    addConfigTo(dom, config, driver_info['configuration'])

    return dom.toprettyxml()

def pathexists(path):
    try:
        os.stat(path)
        return True
    except OSError, inst:
        if inst.errno == errno.EIO:
            raise CommandException(errno.EIO)
        return False

def isdir(path):
    try:
        st = os.stat(path)
        return stat.S_ISDIR(st.st_mode)
    except OSError, inst:
        if inst.errno == errno.EIO:
            raise CommandException(errno.EIO)
        return False

def ismount(path):
    """Test whether a path is a mount point"""
    try:
        s1 = os.stat(path)
        s2 = os.stat(os.path.join(path, '..'))
    except OSError, inst:
        if inst.errno == errno.EIO:
            raise CommandException(errno.EIO)
    dev1 = s1.st_dev
    dev2 = s2.st_dev
    if dev1 != dev2:
        return True     # path/.. on a different device as path
    ino1 = s1.st_ino
    ino2 = s2.st_ino
    if ino1 == ino2:
        return True     # path/.. is the same i-node as path
    return False

def makedirs(name, mode=0777):
    head, tail = os.path.split(name)
    if not tail:
        head, tail = os.path.split(head)
    if head and tail and not pathexists(head):
        makedirs(head, mode)
        if tail == os.curdir:
            return
    os.mkdir(name, mode)
