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

import sys
import mpath_cli
import scsiutil
import os
import time


def list():
    maps = mpath_cli.list_maps()
    for m in maps:
        print m

def wait_for_multipathd():
    for i in range(0,120):
        if mpath_cli.is_working():
            return
        time.sleep(1)
    print "Unable to contact Multipathd daemon"
    sys.exit(-1)

def status():
    for line in (mpath_cli.get_all_topologies()):
        print line

def usage():
    print "Usage:";
    print "%s list" % sys.argv[0]
    print "%s status" % sys.argv[0]


def main():
    if len(sys.argv) < 2:
        usage()
        sys.exit(-1)

    mode=sys.argv[1]

    # Check that multipathd is up and running first
    wait_for_multipathd()

    if mode=="list":
        list()
    elif mode=="status":
        status()
    else:
        usage()
        sys.exit(-1)

if __name__ == "__main__":
    main()
    
