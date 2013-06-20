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

import sys
import os
import util
import re
import glob

def get_luninfo(scsi_id):
    luninfo = {}
    links=glob.glob('/dev/disk/mpInuse/%s-*' % scsi_id)
    if (len(links)):
        alist = links[0].split('/')[-1].split('-')[-1].split(':')
        luninfo['targetID'] = alist[0]
        luninfo['lunnum'] = alist[1]
    return luninfo

def query_pathdata(scsi_id, luninfo):
    pathlistcmd = ["/usr/sbin/mppUtil", "-P"]
    cmd_option_str = luninfo['targetID'] + "," + luninfo['lunnum']
    pathlistcmd.append(cmd_option_str)
    return util.doexec(pathlistcmd)

def query_hbtl(scsi_id):
    luninfo = get_luninfo(scsi_id)
    (rc,stdout,stderr) = query_pathdata(scsi_id, luninfo)
    if rc != 1:
        util.SMlog("Failed to query SCSIid")
        return "-1:-1:-1:-1"
    lines = stdout.split('\n')
    id = re.sub("[H,C,T]"," ",lines[0].split()[0]).split()
    return "%s:%s:%s:%s" % (id[0],id[1],id[2],luninfo['lunnum'])
    
def get_pathinfo(scsi_id, verbose = False):
    luninfo = get_luninfo(scsi_id)
    (rc,stdout,stderr) = query_pathdata(scsi_id, luninfo)
    if verbose:
        print stdout
        return
    lines = stdout.split('\n')
    for line in lines:
            regex = re.compile("TOTAL PATHS:")
            mt = regex.search(line)
            if (mt):
                total_count = line.split(':')[1].strip()
                continue
            regex = re.compile("PATHS UP:")
            mu = regex.search(line)
            if (mu):
                active_count = line.split(':')[1].strip()
                continue

    return (int(total_count), int(active_count))

def usage():
    print "Usage:";
    print "%s pathinfo <scsi_id> Counts" % sys.argv[0]
    print "%s pathinfo <scsi_id> Status" % sys.argv[0]
    print "%s luninfo <scsi_id>" % sys.argv[0]


def main():
    if len(sys.argv) < 3:
        usage()
        sys.exit(-1)

    mode=sys.argv[1]
    scsi_id = sys.argv[2]
    testlinks=glob.glob('/dev/disk/mpInuse/%s-*' % scsi_id)
    if not (len(testlinks)):
        return
    if mode == "luninfo":
        luninfo =  get_luninfo(scsi_id)
        if luninfo:
            print luninfo['lunnum']
    else:
        if mode == "pathinfo":
            if len(sys.argv) < 4:
                usage()
                sys.exit(-1)
            submode = sys.argv[3]
            if (submode == "Counts"):
                (total_count, active_count) = get_pathinfo(scsi_id)
                print "Total: %s, Active: %s" % (total_count, active_count)
            elif (submode == "HBTL"):
                print query_hbtl(scsi_id)
            elif (submode == "Status"):
                get_pathinfo(scsi_id, verbose=True)
            else:
                usage()
                sys.exit(-1)

if __name__ == "__main__":
    main()
