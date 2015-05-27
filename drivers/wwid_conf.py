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
# Manipulation utilities for multipath.conf
#


import fileinput, shutil, sys, re
import os, subprocess
import util, lock

LOCK_TYPE_HOST = "host"
LOCK_NS = "multipath.conf"
# We always expect this file.
# It must be a regular file pointed by /etc/multipath.conf
CONF_FILE = "/etc/multipath.xenserver/multipath.conf"
BELIST_TAG = "blacklist_exceptions"

class WWIDException(Exception):
    def __init__(self, errstr):
        self.errstr = errstr


def edit_wwid(wwid, remove=False):
    """Add a wwid to the list of exceptions or remove if
    remove is set to 1.

    """

    tmp_file = CONF_FILE+"~"
    filt_regex = re.compile('^\s*%s\s*{'%BELIST_TAG)
    wwid_regex = re.compile('^\s*wwid\s+\"%s\"'%wwid)

    conflock = lock.Lock(LOCK_TYPE_HOST, LOCK_NS)
    conflock.acquire()

    try:
        shutil.copy2(CONF_FILE, tmp_file)
    except:
        util.SMlog("Failed to create temp file %s" %(tmp_file))
        raise

    add_mode = True
    for line in fileinput.input(tmp_file, inplace=1):
        if add_mode:
            print line,
        else:
            if wwid_regex.match(line):
                add_mode = True
            else:
                print line,
            continue
            
        if filt_regex.match(line):
            if remove:
                # looking for the line to remove
                add_mode = False
                continue
            else:
	        print "\twwid \"%s\""%wwid

    shutil.move(tmp_file, CONF_FILE)

def is_blacklisted(dev):
    """This function returns True if the device is blacklisted according
    to multipath.conf rules.

    There is also the possibility that the path to the device we are
    checking is unavailable for some reason. In that case,
    is_blacklisted() cannot tell if the device is blacklisted or not
    and a WWIDException is raised.

    It cannot be used to check the current daemon rules because it
    could be running with an old configuration file in memory

    Input:
        dev -- it is any string accepted by "multipath -c". A full path
               is expected.

    Return:
        bool -- True or False, depending on whether the device is
                blacklisted or not.

    Raise:
        WWIDException
    """

    (rc,stdout,stderr) = util.doexec(['/sbin/multipath','-v3','-c',dev])

    if "scope is nul" in stdout:
        raise WWIDException("WARNING: Could not retrieve device's "
                            "WWID. Path {} is down".format(dev))

    # If the device is blacklisted according to multipath.conf, the
    # string "wwid blacklisted" appears in the verbose output.
    # This is a very fragile mechanism and keeps changing.
    # What we want is a method to tell immediately if a device is
    # blacklisted according only to configuration file rules regardless
    # of daemon in-memory configuration.
    return "wwid blacklisted" in stdout


def check_conf_file():
    (rc,stdout,stderr) = util.doexec(['/sbin/multipath','-h'])
    # Ugly way to check for malformed conf file
    if len(stdout):
        util.SMlog("Malformed multipath conf file")
        return 1
    return 0


def usage():
    print "Usage: %s [-r] -d <device path> -w <device wwid>" % sys.argv[0]
    print "Usage: %s -f [-r] -w <device wwid>" % sys.argv[0]
    print "\tAdd a device wwid to multipath.conf whitelist"
    print "\t-r: remove"
    print "\t-f: if provided the operation will be performed anyway"
    print "\t    otherwise it will be after checking <device> is"
    print "\t    blacklisted (or not)"


if __name__ == "__main__":
    import getopt
    from operator import xor

    try:
        opts, args = getopt.getopt(sys.argv[1:], "fd:w:r")
    except getopt.GetoptError:
        usage()
        sys.exit(1)

    remove = False
    device = ""
    force = False
    wwid = ""

    for o, a in opts:
        if o == "-r":
            remove = True
        elif o == "-d":
            device = a
        elif o == "-f":
            force = True
        elif o == "-w":
            wwid = a

    if not wwid:
        usage()
        sys.exit(1)

    if not (device or force):
        usage()
        sys.exit(1)

    try:
        if force or xor(remove, is_blacklisted(device)):
            try:
                edit_wwid(wwid, remove)
            except:
                sys.exit(1)
    except WWIDException as e:
        util.SMlog(e.errstr)

    sys.exit(0)

