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
# Tool to do a vhd-util check on all VHDs belonging to a VG/SR.
# Usage is "./verifyVHDsOnSR.py <sr_uuid>". This tool verifies all the VHDs
# on a VHD based SR. (FC or iSCSI)
#

import os
import sys
import util
import lvutil
import lvhdutil
import vhdutil

from lock import Lock
from refcounter import RefCounter

# Stores the vdi activated, comes handy while deactivating
VHDs_passed = 0
VHDs_failed = 0

def activateVdiChainAndCheck(vhd_info, vg_name):
    global VHDs_passed
    global VHDs_failed
    activated_list = []
    vhd_path = os.path.join(lvhdutil.VG_LOCATION, vg_name, vhd_info.path)
    if not activateVdi(
                       vg_name.lstrip(lvhdutil.VG_PREFIX), 
                       vhd_info.uuid, 
                       vhd_path):
        # If activation fails, do not run check, also no point on running 
        # check on the VDIs down the chain
        util.SMlog("VHD activate failed for %s, skipping rest of VDH chain" % 
                    vg_name)
        return activated_list

    activated_list.append([vhd_info.uuid, vhd_path])
    # Do a vhdutil check with -i option, to ignore error in primary
    if not vhdutil.check(vhd_path, True):
        util.SMlog("VHD check for %s failed, continuing with the rest!" % vg_name)
        VHDs_failed += 1
    else:
        VHDs_passed += 1
    
    if hasattr(vhd_info, 'children'):
        for vhd_info_sub in vhd_info.children:
            activated_list.extend(activateVdiChainAndCheck(vhd_info_sub, vg_name))

    return activated_list

def activateVdi(sr_uuid, vdi_uuid, vhd_path):
    name_space = lvhdutil.NS_PREFIX_LVM + sr_uuid
    lock = Lock(vdi_uuid, name_space)
    lock.acquire()
    try:
        count = RefCounter.get(vdi_uuid, False, name_space)
        if count == 1:
            try:
                lvutil.activateNoRefcount(vhd_path, False)
            except Exception, e:
                util.SMlog("  lv activate failed for %s with error %s" % 
                           (vhd_path, str(e)))
                RefCounter.put(vdi_uuid, True, name_space)
                return False
    finally:
        lock.release()

    return True

def deactivateVdi(sr_uuid, vdi_uuid, vhd_path):
    name_space = lvhdutil.NS_PREFIX_LVM + sr_uuid
    lock = Lock(vdi_uuid, name_space)
    lock.acquire()
    try:
        count = RefCounter.put(vdi_uuid, False, name_space)
        if count > 0:
            return
        try:
            lvutil.deactivateNoRefcount(vhd_path)
        except Exception, e:
            util.SMlog("  lv de-activate failed for %s with error %s" %
                       (vhd_path, str(e)))
            RefCounter.get(vdi_uuid, False, name_space)
    finally:
        lock.release()

def checkAllVHD(sr_uuid):
    activated_list = []
    vhd_trees = []
    VHDs_total = 0

    vg_name = lvhdutil.VG_PREFIX + sr_uuid
    pattern = "%s*" % lvhdutil.LV_PREFIX[vhdutil.VDI_TYPE_VHD]

    # Do a vhd scan and gets all the VHDs
    vhds = vhdutil.getAllVHDs(pattern, lvhdutil.extractUuid, vg_name)
    VHDs_total = len(vhds)

    # Build VHD chain, that way it will be easier to activate all the VHDs
    # that belong to one chain, do check on the same and then deactivate
    for vhd in vhds:
        if vhds[vhd].parentUuid:
            parent_VHD_info = vhds.get(vhds[vhd].parentUuid)
            if not hasattr(parent_VHD_info, 'children'):
                parent_VHD_info.children = []
            parent_VHD_info.children.append(vhds[vhd])
        else:
            vhd_trees.append(vhds[vhd])
    
    # If needed, activate VHDs belonging to each VDI chain,  do a check on 
    # all VHDs and then set the state back.
    for vhd_chain in vhd_trees:
        activated_list = activateVdiChainAndCheck(vhd_chain, vg_name)

        #Deactivate the LVs, states are maintained by Refcounter
        for item in activated_list:
            deactivateVdi(sr_uuid, item[0], item[1])

    print("VHD check passed on %d, failed on %d, not run on %d" % 
          (VHDs_passed, VHDs_failed, VHDs_total - (VHDs_passed + VHDs_failed)))

if __name__ == '__main__':
    if len(sys.argv) == 1:
        print("Usage:")
        print("/opt/xensource/sm/verifyVHDsOnSR.py <sr_uuid>")
    else:
        checkAllVHD(sys.argv[1])

