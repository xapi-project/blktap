#! /usr/bin/env python
# Copyright (c) 2008 Citrix Systems, Inc. All use and distribution of this
# copyrighted material is governed by and subject to terms and conditions
# as licensed by Citrix Systems, Inc. All other rights reserved.
# Xen, XenSource and XenEnterprise are either registered trademarks or
# trademarks of Citrix Systems, Inc. in the United States and/or other 
# countries.

import sshutil
import getpass
import sys, os
import re
import time

start_time = time.time()
#IP address of the EqualLogic Storage group
host=sys.argv[1]
#Username of the group admin user of the EqualLogic Storage group
user = sys.argv[2]
#EqualLogic Storage group CLI command to be executed: Example: "vol show"
#to be specified as a string in quotes.
command = sys.argv[3]
password = getpass.getpass()
conn_cred = [host, user, password]

conn = sshutil.SSHSession(conn_cred)
conn.connect()
(rc, fw_supported, errlog, errmsg) = sshutil.check_version(conn)
if (rc):
        print errmsg
        for id in range(len(errlog)):
                print errlog[id]
else:
        if not fw_supported :
                print "FW version not supported"            
                sys.exit()

(rc, output, errlog, errmsg) = conn.command(command)
if (rc):
        print errmsg
        for id in range(len(errlog)):
                print errlog[id]
else:
        for line in output: print line

#Comment the next line to run the series of example test cases below.
sys.exit()

print "\nListing SR volumes"
(rc, vol_list, errlog, errmsg) = sshutil.list_SR_vols(conn, "XenStorage")
if (rc):
        print errmsg
        for id in range(len(errlog)):
               print errlog[id]
else:
        print vol_list
        for id1 in range(len(vol_list)):
                print "\nListing snapshots for volume:%s"% vol_list[id1]
                (rc, snap_list, errlog, errmsg) = sshutil.list_VDI_snaps(conn, vol_list[id1])
                if (rc):
                        print errmsg
                        for id2 in range(len(errlog)):
                                print errlog[id2]
                else:
                        print snap_list
                        for id3 in range(len(snap_list)):
                            print "\nListing snapshot connection details for %s"% snap_list[id3]
                            (rc, output, errlog, errmsg) = sshutil.snap_conn_detail(conn, vol_list[id1], snap_list[id3])
                            if (rc):
                                print errmsg
                                for id4 in range(len(errlog)):
                                    print errlog[id4]
                            else:
                                print output
#sys.exit()

#set the loop count below to run the tests repeatedly to create stress
loop_count = 1
while True:
    if (loop_count == 0):
            break
    print "\nListing all pools in the group"
    (rc, output, errlog, errmsg) = sshutil.all_StoragePools(conn)
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output
    #sys.exit()

    print "\nListing pool details for default pool"
    (rc, output, errlog, errmsg) = sshutil.pool_detail(conn, "default")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output
    #sys.exit()

    print "\nCreating accessvol in the SR"
    (rc, output, iSCSIname, errlog, errmsg) = sshutil.vol_create(conn, "accessvol", "10GB", "thin", "cece3456-bad2-4fc2-9142-990d3d920fb7", "offline", 100, "volume-offline", "default") 
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output
            print "iSCSI name: %s"% iSCSIname

    initiator_name = "iqn.2006-04.com.example:4567acdb"
    print "\nCreating access control record for accessvol with initiator %s"% initiator_name
    (rc, output, errlog, errmsg) = sshutil.vol_create_access_rec_by_initiator(conn, "accessvol", initiator_name)
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    initiator_name = "iqn.2006-04.com.example:4567acdb"
    print "\nDeleting access information for initiator %s from accessvol"% initiator_name
    (rc, output, errlog, errmsg) = sshutil.vol_delete_access_rec_by_initiator(conn, "accessvol", initiator_name)
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            for id in range(len(output)):
                    print output[id]
    #sys.exit()

    print "\nDeleting accessvol from SR"
    (rc, output, errlog, errmsg) = sshutil.vol_delete(conn, "accessvol") 
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    vdi_name = "XenStorage0ff867ef31210e7c5497f5c34916727d22267eb723c04479a063"
    print "\nCreating VDI:%s in the SR" % vdi_name
    (rc, output, iSCSIname, errlog, errmsg) = sshutil.vol_create(conn, vdi_name, "10GB", "thin", "22267eb7-23c0-4479-a063-f5c34916727d", "offline", 100, "volume-offline", "default") 
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output
            print "iSCSI name: %s"% iSCSIname

    print "\nCreating snapshot of VDI:%s in the SR" % vdi_name
    (rc, errlog, errmsg) = sshutil.vol_create_snapshot(conn, vdi_name, "bebe1234-bad2-4fc2-9142-990d3d920fb7", "eeff5678-bad2-4fc2-9142-990d3d92aaaa", "SNAPSHOT")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nListing snapshots of VDI: %s"% vdi_name
    (rc, snap_list, errlog, errmsg) = sshutil.list_VDI_snaps(conn, vdi_name)
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print snap_list
            for id in range(len(snap_list)):
                print "\nListing snapshot details for %s"% snap_list[id]
                (rc, output, errlog, errmsg) = sshutil.snap_detail(conn, vdi_name, snap_list[id])
                if (rc):
                    print errmsg
                    for id in range(len(errlog)):
                        print errlog[id]
                else:
                    print output
    #sys.exit()

    snapname= "XenStorage" + "bebe1234-bad2-4fc2-9142-990d3d920fb7".replace("-", "") + "eeff5678-bad2-4fc2-9142-990d3d92aaaa".replace("-", "")[:20]

    print "\nDeleting snapshot of VDI:%s from SR" % vdi_name
    (rc, output, errlog, errmsg) = sshutil.snap_delete(conn, vdi_name, snapname) 
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    print "\nDeleting VDI:%s from SR" % vdi_name
    (rc, output, errlog, errmsg) = sshutil.vol_delete(conn, vdi_name) 
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    print "\nCreating bigvol in the SR"
    (rc, output, iSCSIname, errlog, errmsg) = sshutil.vol_create(conn, "bigvol", "10GB", "thin", "bebe1234-bad2-4fc2-9142-990d3d920fb7", "offline", 100, "volume-offline", "default") 
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output
            print "iSCSI name: %s"% iSCSIname

    print "\nListing volume details for bigvol"
    (rc, output, errlog, errmsg) = sshutil.vol_detail(conn, "bigvol")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    print "\nBringing bigvol online"
    (rc, errlog, errmsg) = sshutil.vol_online(conn, "bigvol")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nShrinking bigvol to 8GB"
    (rc, errlog, errmsg) = sshutil.vol_shrink(conn, "bigvol", "8GB")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nBringing bigvol offline"
    (rc, errlog, errmsg) = sshutil.vol_offline(conn, "bigvol")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nShrinking bigvol to 8GB"
    (rc, errlog, errmsg) = sshutil.vol_shrink(conn, "bigvol", "8GB")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]


    print "\nListing volume details for bigvol"
    (rc, output, errlog, errmsg) = sshutil.vol_detail(conn, "bigvol")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    print "\nBringing bigvol online"
    (rc, errlog, errmsg) = sshutil.vol_online(conn, "bigvol")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nGrowing bigvol to 12GB"
    (rc, errlog, errmsg) = sshutil.vol_grow(conn, "bigvol", "12GB")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nListing volume details for bigvol"
    (rc, output, errlog, errmsg) = sshutil.vol_detail(conn, "bigvol")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output
    #sys.exit()

    print "\nDeleting bigvol from SR"
    (rc, output, errlog, errmsg) = sshutil.vol_delete(conn, "bigvol")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    print "\nBringing bigvol offline"
    (rc, errlog, errmsg) = sshutil.vol_offline(conn, "bigvol")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nDeleting bigvol from SR"
    (rc, output, errlog, errmsg) = sshutil.vol_delete(conn, "bigvol")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    print "\nListing SR volumes- Checking if myvol6 exists"
    (rc, vol_list, errlog, errmsg) = sshutil.list_SR_vols(conn, "myvol")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print vol_list

    print "\nCreating myvol6 in the SR"
    (rc, output, iSCSIname, errlog, errmsg) = sshutil.vol_create(conn, "myvol6", "5GB", "none", "bebe1234-bad2-4fc2-9142-990d3d920fb7", "offline", 100, "volume-offline", "default") 
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output
            print "iSCSI name: %s"% iSCSIname

    print "\nCreating clone of myvol6 in the SR"
    (rc, output, iscsiname, errlog, errmsg) = sshutil.vol_create_clone(conn, "myvol6", "bebe1234-bad2-4fc2-9142-990d3d920fb7", "ddbb7698-bad2-4fc2-9142-990d3d920fb7", "UUID-ddbb7698-bad2-4fc2-9142-990d3d920fb7:DELETED-False") 
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output
            print "iSCSI name: %s"% iSCSIname

    clone_name= "XenStorage" + "bebe1234-bad2-4fc2-9142-990d3d920fb7".replace("-", "") + "ddbb7698-bad2-4fc2-9142-990d3d920fb7".replace("-", "")[:20]
    print "\nBringing clone %s online"% clone_name
    (rc, errlog, errmsg) = sshutil.vol_online(conn, clone_name)
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nListing volume details for myvol6"
    (rc, output, errlog, errmsg) = sshutil.vol_detail(conn, "myvol6")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    print "\nListing SR volumes"
    (rc, vol_list, errlog, errmsg) = sshutil.list_SR_vols(conn, "myvol")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print vol_list

    print "\nBringing myvol6 online"
    (rc, errlog, errmsg) = sshutil.vol_online(conn, "myvol6")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nBringing myvol6 offline"
    (rc, errlog, errmsg) = sshutil.vol_offline(conn, "myvol6")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nCreating access control record for myvol6"
    (rc, output, errlog, errmsg) = sshutil.vol_create_access_rec_by_initiator(conn, "myvol6", "iqn.2008-04.com.example:12345a")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    print "\nBringing myvol6 online"
    (rc, errlog, errmsg) = sshutil.vol_online(conn, "myvol6")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nCreating a snapshot of myvol6"
    snapname= "XenStorage" + "bebe1234-bad2-4fc2-9142-990d3d920fb7".replace("-", "") + "ddbb5678-bad2-4fc2-9142-990d3d92aaaa".replace("-", "")[:20]
    (rc, errlog, errmsg) = sshutil.vol_create_snapshot(conn, "myvol6", "bebe1234-bad2-4fc2-9142-990d3d920fb7", "ddbb5678-bad2-4fc2-9142-990d3d92aaaa", "SNAPSHOT")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print "\nCreating a clone form snapshot %s of vol myvol6"% snapname
            (rc, output, iscsiname, errlog, errmsg) = sshutil.vol_create_clone_from_snapshot(conn, "myvol6", snapname, "bebe1234-bad2-4fc2-9142-990d3d920fb7", "eeff9012-bad2-4fc2-9142-990d3d92cccc", "UUID-eeff9012-bad2-4fc2-9142-990d3d92cccc:DELETED-False")
            if (rc):
                    print errmsg
                    for id in range(len(errlog)):
                            print errlog[id]
            else:
                    print output
                    print "iSCSI name of the clone is: %s"% iscsiname

    print "\nListing snapshot details for %s"% snapname
    (rc, output, errlog, errmsg) = sshutil.snap_detail(conn, "myvol6", snapname)
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    clone_name= "XenStorage" + "bebe1234-bad2-4fc2-9142-990d3d920fb7".replace("-", "") + "ddbb7698-bad2-4fc2-9142-990d3d920fb7".replace("-", "")[:20]

    print "\nBringing clone of myvol6 offline"
    (rc, errlog, errmsg) = sshutil.vol_offline(conn, clone_name)
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nDeleting clone of myvol6: %s from SR"% clone_name
    (rc, output, errlog, errmsg) = sshutil.vol_delete(conn, clone_name)
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    snap_clone= "XenStorage" + "bebe1234-bad2-4fc2-9142-990d3d920fb7".replace("-", "") + "eeff9012-bad2-4fc2-9142-990d3d92cccc".replace("-", "")[:20]
    print "\nDeleting snap-clone of myvol6: %s from SR"% snap_clone
    (rc, output, errlog, errmsg) = sshutil.vol_delete(conn, snap_clone)
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    print "\nDeleting snapshot of myvol6 from SR"
    (rc, output, errlog, errmsg) = sshutil.snap_delete(conn, "myvol6", snapname) 
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    print "\nBringing myvol6 offline"
    (rc, errlog, errmsg) = sshutil.vol_offline(conn, "myvol6")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]

    print "\nDeleting myvol6 from SR"
    (rc, output, errlog, errmsg) = sshutil.vol_delete(conn, "myvol6")
    if (rc):
            print errmsg
            for id in range(len(errlog)):
                    print errlog[id]
    else:
            print output

    print "Total time elapsed: %is"% (time.time() - start_time)

    loop_count = loop_count - 1

conn.close()
