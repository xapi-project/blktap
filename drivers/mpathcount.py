#!/usr/bin/python

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

import util
import time, os, sys, re
import xs_errors
import lock
import mpath_cli
import mpp_luncheck
import mpp_mpathutil
import glob

supported = ['iscsi','lvmoiscsi','rawhba','lvmohba', 'ocfsohba', 'ocfsoiscsi', 'netapp','cslg']

LOCK_TYPE_HOST = "host"
LOCK_NS1 = "mpathcount1"
LOCK_NS2 = "mpathcount2"

MP_INUSEDIR = "/dev/disk/mpInuse"
mpp_path_update = False
match_bySCSIid = False

util.daemon()
if len(sys.argv) == 3:
    match_bySCSIid = True
    SCSIid = sys.argv[1]
    mpp_path_update = True
    mpp_entry = sys.argv[2]

# We use flocks to ensure that only one process 
# executes at any one time, however we must make
# sure that any subsequent changes are always 
# correctly updated, so we allow an outstanding
# process to queue behind the running one.
mpathcountlock = lock.Lock(LOCK_TYPE_HOST, LOCK_NS1)
mpathcountqueue = lock.Lock(LOCK_TYPE_HOST, LOCK_NS2)
util.SMlog("MPATH: Trying to acquire the lock")
if mpp_path_update:
    mpathcountlock.acquire()
elif not mpathcountlock.acquireNoblock():
    if not mpathcountqueue.acquireNoblock():
        # There is already a pending update
        # so safe to exit
        sys.exit(0)
    # We acquired the pending queue lock
    # so now wait on the main lock
    mpathcountlock.acquire()
    mpathcountqueue.release()

util.SMlog("MPATH: I get the lock")

cached_DM_maj = None
def get_dm_major():
    global cached_DM_maj
    if not cached_DM_maj:
        try:
            line = filter(lambda x: x.endswith('device-mapper\n'), open('/proc/devices').readlines())
            cached_DM_maj = int(line[0].split()[0])
        except:
            pass
    return cached_DM_maj

def mpc_exit(session, code):
    if session is not None:
        try:
            session.xenapi.logout()
        except:
            pass
    sys.exit(code)

def match_host_id(s):
    regex = re.compile("^INSTALLATION_UUID")
    return regex.search(s, 0)

def get_localhost_uuid():
    filename = '/etc/xensource-inventory'
    try:
        f = open(filename, 'r')
    except:
        raise xs_errors.XenError('EIO', \
              opterr="Unable to open inventory file [%s]" % filename)
    domid = ''
    for line in filter(match_host_id, f.readlines()):
        domid = line.split("'")[1]
    return domid

def match_dmpLUN(s):
    regex = re.compile("[0-9]*:[0-9]*:[0-9]*:[0-9]*")
    return regex.search(s, 0)

def match_pathup(s):
    s = re.sub('\]',' ',re.sub('\[','',s)).split()
    # The new multipath has a different output. Fixed it
    dm_status = s[-2]
    path_status = s[-3]
    # path_status is more reliable, at least for failures initiated or spotted by multipath
    # To be tested for failures during high I/O when dm should spot errors first
    for val in [path_status]:
        if val in ['faulty','shaky','failed']:
            return False
    return True

def _tostring(l):
    return str(l)

def get_path_count(SCSIid, active=True):
    count = 0
    if (mpp_luncheck.is_RdacLun(SCSIid)):
        (total_count, active_count) = mpp_mpathutil.get_pathinfo(SCSIid)
        return (total_count, active_count)
    lines = mpath_cli.get_topology(SCSIid)
    for line in filter(match_dmpLUN,lines):
        if not active:
            count += 1
        elif match_pathup(line):
            count += 1
    return count

def get_root_dev_major():
    buf = os.stat('/dev/root')
    devno = buf.st_rdev
    return os.major(devno)

# @key:     key to update
# @SCSIid:  SCSI id of multipath map
# @entry:   string representing previous value
# @remove:  callback to remove key
# @add:     callback to add key/value pair
def update_config(key, SCSIid, entry, remove, add, mpp_path_update = False):
    if mpp_path_update:
        remove('multipathed')
        remove(key)
        remove('MPPEnabled')
        add('MPPEnabled','true')
        add('multipathed','true')
        add(key,str(entry))
        return

    rdaclun = False
    if (mpp_luncheck.is_RdacLun(SCSIid)):
        rdaclun = True
        pathlist = glob.glob('/dev/disk/mpInuse/%s-*' % SCSIid)
        path = pathlist[0]
    else:
        path = MP_INUSEDIR + "/" + SCSIid
    util.SMlog("MPATH: Updating entry for [%s], current: %s" % (SCSIid,entry))
    if os.path.exists(path):
        if rdaclun:
            (total, count) = get_path_count(SCSIid)
        else:
            count = get_path_count(SCSIid)
            total = get_path_count(SCSIid, active=False)
        max = 0
	if len(entry) != 0:
            try:
                p = entry.strip('[')
                p = p.strip(']')
                q = p.split(',')
                max = int(q[1])
            except:
                pass
        if total > max:
            max = total
        newentry = [count, max]
        if str(newentry) != entry:
            remove('multipathed')
            remove(key)
            if rdaclun:
                remove('MPPEnabled')
                add('MPPEnabled','true')
            add('multipathed','true')
            add(key,str(newentry))
            util.SMlog("MPATH: \tSet val: %s" % str(newentry))

def get_SCSIidlist(devconfig, sm_config):
    SCSIidlist = []
    if sm_config.has_key('SCSIid'):
        SCSIidlist = sm_config['SCSIid'].split(',')
    elif devconfig.has_key('SCSIid'):
        SCSIidlist.append(devconfig['SCSIid'])
    else:
        for key in sm_config:
            if util._isSCSIid(key):
                SCSIidlist.append(re.sub("^scsi-","",key))
    return SCSIidlist

try:
    session = util.get_localAPI_session()
except:
    print "Unable to open local XAPI session"
    sys.exit(-1)

localhost = session.xenapi.host.get_by_uuid(get_localhost_uuid())
# Check whether DMP Multipathing is enabled (either for root dev or SRs)
try:
    if get_root_dev_major() != get_dm_major():
        hconf = session.xenapi.host.get_other_config(localhost)
        assert(hconf['multipathing'] == 'true')
        assert(hconf['multipathhandle'] == 'dmp')
except:
    mpc_exit(session,0)

# Check root disk if multipathed
try:
    if get_root_dev_major() == get_dm_major():
        def _remove(key):
            session.xenapi.host.remove_from_other_config(localhost,key)
        def _add(key, val):
            session.xenapi.host.add_to_other_config(localhost,key,val)
        config = session.xenapi.host.get_other_config(localhost)
        maps = mpath_cli.list_maps()
        # first map will always correspond to the root dev, dm-0
        assert(len(maps) > 0)
        i = maps[0]
        if (not match_bySCSIid) or i == SCSIid:
            util.SMlog("Matched SCSIid %s, updating " \
                       " Host.other-config:mpath-boot " % i)
            key="mpath-boot"
            if not config.has_key(key):
                update_config(key, i, "", _remove, _add)
            else:
                update_config(key, i, config[key], _remove, _add)
except:
    util.SMlog("MPATH: Failure updating Host.other-config:mpath-boot db")
    mpc_exit(session, -1)

try:
    pbds = session.xenapi.PBD.get_all_records_where("field \"host\" = \"%s\"" % localhost)
except:
    mpc_exit(session,-1)

try:
    for pbd in pbds:
        def remove(key):
            session.xenapi.PBD.remove_from_other_config(pbd,key)
        def add(key, val):
            session.xenapi.PBD.add_to_other_config(pbd,key,val)
        record = pbds[pbd]
        config = record['other_config']
        SR = record['SR']
        srtype = session.xenapi.SR.get_type(SR)
        if srtype in supported:
            devconfig = record["device_config"]
            sm_config = session.xenapi.SR.get_sm_config(SR)
            SCSIidlist = get_SCSIidlist(devconfig, sm_config)
            if not len(SCSIidlist):
                continue
            for i in SCSIidlist:
                if match_bySCSIid and i != SCSIid:
                    continue
                util.SMlog("Matched SCSIid, updating %s" % i)
                key = "mpath-" + i
                if mpp_path_update:
                    util.SMlog("Matched SCSIid, updating entry %s" % str(mpp_entry))
                    update_config(key, i, mpp_entry, remove, add, mpp_path_update)
                else:
                    if not config.has_key(key):
                        update_config(key, i, "", remove, add)
                    else:
                        update_config(key, i, config[key], remove, add)
except:
    util.SMlog("MPATH: Failure updating db")
    mpc_exit(session, -1)
    
util.SMlog("MPATH: Update done")

mpc_exit(session,0)
