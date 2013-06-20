#! /usr/bin/env python
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

import sys, os
import glob

DEVBYMPPPATH = "/dev/disk/by-mpp"
def is_RdacLun(scsi_id):
    path = os.path.join(DEVBYMPPPATH,"%s" % scsi_id)
    mpppath = glob.glob(path)
    if len(mpppath):
        return True
    else:
        return False

def usage():
    print "Usage:";
    print "%s is_rdaclun <scsi_id>" % sys.argv[0]

def main():
    if len(sys.argv) < 3:
        usage()
        sys.exit(-1)

    scsi_id = sys.argv[2]
    mode = sys.argv[1]

    if mode == "is_rdaclun":
        if (is_RdacLun(scsi_id)):
            print "It is a RDAC Lun"
            return True
        else:
            print "It is not a RDAC Lun"
            return False
    else:
        usage()
        sys.exit(-1)
if __name__ == "__main__":
    main()
