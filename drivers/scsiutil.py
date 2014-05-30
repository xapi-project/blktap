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
# Miscellaneous scsi utility functions
#

import util, SR
import os
import re
import xs_errors
import base64
import time
import errno
import glob
import mpath_cli

PREFIX_LEN = 4
SUFFIX_LEN = 12
SECTOR_SHIFT = 9

def gen_hash(st, len):
    hs = 0
    for i in st:
        hs = ord(i) + (hs << 6) + (hs << 16) - hs
    return str(hs)[0:len]

def gen_uuid_from_serial(iqn, serial):
    if len(serial) < SUFFIX_LEN:
        raise util.CommandException(1)
    prefix = gen_hash(iqn, PREFIX_LEN)
    suffix = gen_hash(serial, SUFFIX_LEN)
    str = prefix.encode("hex") + suffix.encode("hex")
    return str[0:8]+'-'+str[8:12]+'-'+str[12:16]+'-'+str[16:20]+'-'+str[20:32]

def gen_serial_from_uuid(iqn, uuid):
    str = uuid.replace('-','')
    prefix = gen_hash(iqn, PREFIX_LEN)
    if str[0:(PREFIX_LEN * 2)].decode("hex") != prefix:
        raise util.CommandException(1)
    return str[(PREFIX_LEN * 2):].decode("hex")

def getsize(path):
    dev = getdev(path)
    sysfs = os.path.join('/sys/block',dev,'size')
    size = 0
    if os.path.exists(sysfs):
        try:
            f=open(sysfs, 'r')
            size = (long(f.readline()) << SECTOR_SHIFT)
            f.close()
        except:
            pass
    return size

def getuniqueserial(path):
    dev = getdev(path)
    output = gen_rdmfile()
    try:
        cmd = ["md5sum"]
        txt = util.pread3(cmd, getSCSIid(path))
        return txt.split(' ')[0]
    except:
        return ''

def gen_uuid_from_string(str):
    if len(str) < (PREFIX_LEN + SUFFIX_LEN):
        raise util.CommandException(1)
    return str[0:8]+'-'+str[8:12]+'-'+str[12:16]+'-'+str[16:20]+'-'+str[20:32]

def SCSIid_sanitise(str):
    text = re.sub("^\s+","",str)
    return re.sub("\s+","_",text)

def getSCSIid(path):
    dev = rawdev(path)
    cmd = ["scsi_id", "-g", "-s", "/block/%s" % dev]
    return SCSIid_sanitise(util.pread2(cmd)[:-1])

def compareSCSIid_2_6_18(SCSIid, path):
    serial = getserial(path)
    len_serial = len(serial)
    if (len_serial == 0 ) or (len_serial > (len(SCSIid) - 1)):
        return False
    list_SCSIid = list(SCSIid)
    list_serial = list_SCSIid[1:(len_serial + 1)]
    serial_2_6_18 = ''.join(list_serial)
    if (serial == serial_2_6_18):
        return True
    else:
        return False

def getserial(path):
    dev = os.path.join('/dev',getdev(path))
    try:
        cmd = ["sginfo", "-s", dev]
        text = re.sub("\s+","",util.pread2(cmd))
    except:
        raise xs_errors.XenError('EIO', \
              opterr='An error occured querying device serial number [%s]' \
                           % dev)
    try:
        return text.split("'")[1]
    except:
        return ''

def getmanufacturer(path):
    cmd = ["sginfo", "-M", path]
    try:
        for line in filter(match_vendor, util.pread2(cmd).split('\n')):
            return line.replace(' ','').split(':')[-1]
    except:
        return ''

def cacheSCSIidentifiers():
    SCSI = {}
    SYS_PATH = "/dev/disk/by-scsibus/*"
    for node in glob.glob(SYS_PATH):
        if not re.match('.*-\d+:\d+:\d+:\d+$', node):
            continue
        dev = os.path.realpath(node)
        HBTL = os.path.basename(node).split("-")[-1].split(":")
        line = "NONE %s %s %s %s 0 %s" % \
               (HBTL[0],HBTL[1],HBTL[2],HBTL[3],dev)
        ids = line.split()
        SCSI[ids[6]] = ids
    return SCSI

def scsi_dev_ctrl(ids, cmd):
    f = -1
    for i in range(0,10):
        try:
            str = "scsi %s-single-device %s %s %s %s" % \
                  (cmd, ids[1],ids[2],ids[3],ids[4])
            util.SMlog(str)
            f=open('/proc/scsi/scsi', 'w')
            print >>f, str
            f.close()
            return
        except IOError, e:
            util.SMlog("SCSI_DEV_CTRL: Failure, %s [%d]" % (e.strerror,e.errno))
            if f >= 0:
                f.close()
            if e.errno == errno.ENXIO:
                util.SMlog("Device has disappeared already")
                return
            f = -1
            time.sleep(6)
            continue
    raise xs_errors.XenError('EIO', \
            opterr='An error occured during the scsi operation')

def getdev(path):
    realpath = os.path.realpath(path)
    if match_dm(realpath):
        newpath = realpath.replace("/dev/mapper/","/dev/disk/by-id/scsi-")
    else:
        newpath = path
    return os.path.realpath(newpath).split('/')[-1]

def get_devices_by_SCSIid(SCSIid):
    devices = os.listdir(os.path.join('/dev/disk/by-scsid', SCSIid))
    if 'mapper' in devices:
        devices.remove('mapper')
    return devices

def rawdev(dev):
    return re.sub("[0-9]*$","",getdev(dev))

def getSessionID(path):
    for line in filter(match_session, util.listdir(path)):
        return line.split('-')[-1]

def match_session(s):
    regex = re.compile("^SESSIONID-")
    return regex.search(s, 0)

def match_vendor(s):
    regex = re.compile("^Vendor:")
    return regex.search(s, 0)

def match_dm(s):
    regex = re.compile("mapper/")
    return regex.search(s, 0)    
    
def match_sd(s):
    regex = re.compile("/dev/sd")
    return regex.search(s, 0)

def _isSCSIdev(dev):
    if match_dm(dev):
        path = dev.replace("/dev/mapper/","/dev/disk/by-id/scsi-")
    else:
        path = dev
    return match_sd(os.path.realpath(path))

def gen_rdmfile():
    return "/tmp/%s" % util.gen_uuid()

def add_serial_record(session, sr_ref, devstring):
    try:
        conf = session.xenapi.SR.get_sm_config(sr_ref)
        conf['devserial'] = devstring
        session.xenapi.SR.set_sm_config(sr_ref, conf)
    except:
        pass

def get_serial_record(session, sr_ref):
    try:
        conf = session.xenapi.SR.get_sm_config(sr_ref)
        return conf['devserial']
    except:
        return ""

def devlist_to_serialstring(devlist):
    serial = ''
    for dev in devlist:
        try:
            devserial = "scsi-%s" % getSCSIid(dev)
            if not len(devserial) > 0:
                continue
            if len(serial):
                serial += ','
            serial += devserial
        except:
            pass
    
    return serial

def gen_synthetic_page_data(uuid):
    # For generating synthetic page data for non-raw LUNs
    # we set the vendor ID to XENSRC
    # Note that the Page 80 serial number must be limited
    # to 16 characters
    page80 = ""
    page80 += "\x00\x80"
    page80 += "\x00\x12"
    page80 += uuid[0:16]
    page80 += "  "
    
    page83 = ""
    page83 += "\x00\x83"
    page83 += "\x00\x31"
    page83 += "\x02\x01\x00\x2d"
    page83 += "XENSRC  "
    page83 += uuid
    page83 += " "
    return ["",base64.b64encode(page80),base64.b64encode(page83)]
    
def gen_raw_page_data(path):
    default = ""
    page80 = ""
    page83 = ""
    try:
        cmd = ["sg_inq", "-r", path]
        text = util.pread2(cmd)
        default = base64.b64encode(text)
            
        cmd = ["sg_inq", "--page=0x80", "-r", path]
        text = util.pread2(cmd)
        page80 = base64.b64encode(text)
            
        cmd = ["sg_inq", "--page=0x83", "-r", path]
        text = util.pread2(cmd)
        page83 = base64.b64encode(text)
    except:
        pass
    return [default,page80,page83]

def update_XS_SCSIdata(vdi_uuid, data):
        # XXX: PR-1255: passing through SCSI data doesn't make sense when
        # it will change over storage migration. It also doesn't make sense
        # to preserve one array's identity and copy it when a VM moves to
        # a new array because the drivers in the VM may attempt to contact
        # the original array, fail and bluescreen.

        xenstore_data = {}
        xenstore_data["vdi-uuid"]=vdi_uuid
        if len(data[0]):
            xenstore_data["scsi/0x12/default"]=data[0]

        if len(data[1]):
            xenstore_data["scsi/0x12/0x80"]=data[1]

        if len(data[2]):
            xenstore_data["scsi/0x12/0x83"]=data[2]

        return xenstore_data

def rescan(ids, fullrescan=True):
    for id in ids:
        refresh_HostID(id, fullrescan)

def _genArrayIdentifier(dev):
    try:
        cmd = ["sg_inq", "--page=0xc8", "-r", dev]
        id = util.pread2(cmd)
        return id.encode("hex")[180:212]
    except:
        return ""


def _genHostList(procname):
    # loop through and check all adapters
    ids = []
    try:
        for dir in util.listdir('/sys/class/scsi_host'):
            filename = os.path.join('/sys/class/scsi_host',dir,'proc_name')
            if os.path.exists(filename):
                f = open(filename, 'r')
                if f.readline().find(procname) != -1:
                    ids.append(dir.replace("host",""))
                f.close()
    except:
        pass
    return ids

def _genReverseSCSIidmap(SCSIid, pathname="scsibus"):
    util.SMlog("map_by_scsibus: sid=%s" % SCSIid)

    devices = []
    for link in glob.glob('/dev/disk/by-%s/%s-*' % (pathname,SCSIid)):
        realpath = os.path.realpath(link)
        if os.path.exists(realpath):
            devices.append(realpath)
    return devices

def _genReverseSCSidtoLUNidmap(SCSIid):
    devices = []
    for link in glob.glob('/dev/disk/by-scsibus/%s-*' % SCSIid):
        devices.append(link.split('-')[-1])
    return devices

def _dosgscan():
    regex=re.compile("([^:]*):\s+scsi([0-9]+)\s+channel=([0-9]+)\s+id=([0-9]+)\s+lun=([0-9]+)")
    scan=util.pread2(["/usr/bin/sg_scan"]).split('\n')
    sgs=[]
    for line in scan:
        m=regex.match(line)
        if m:
            device=m.group(1)
            host=m.group(2)
            channel=m.group(3)
            sid=m.group(4)
            lun=m.group(5)
            sgs.append([device,host,channel,sid,lun])
    return sgs

def refresh_HostID(HostID, fullrescan):
    LUNs = glob.glob('/sys/class/scsi_disk/%s*' % HostID)
    li = []
    for l in LUNs:
        chan = re.sub(":[0-9]*$",'',os.path.basename(l))
        if chan not in li:
            li.append(chan)

    if len(li) and not fullrescan:
        for c in li:
            if not refresh_scsi_channel(c):
                fullrescan = True

    if fullrescan:
        util.SMlog("Full rescan of HostID %s" % HostID)
        path = '/sys/class/scsi_host/host%s/scan' % HostID
        if os.path.exists(path):
            try:
                scanstring = "- - -"
                f=open(path, 'w')
                f.write('%s\n' % scanstring)
                f.close()
                if len(li):
                    # Channels already exist, allow some time for
                    # undiscovered LUNs/channels to appear
                    time.sleep(2)
            except:
                pass
        # Host Bus scan issued, now try to detect channels
        if util.wait_for_path("/sys/class/scsi_disk/%s*" % HostID, 5):
            # At least one LUN is mapped
            LUNs = glob.glob('/sys/class/scsi_disk/%s*' % HostID)
            li = []
            for l in LUNs:
                chan = re.sub(":[0-9]*$",'',os.path.basename(l))
                if chan not in li:
                    li.append(chan)
            for c in li:
                refresh_scsi_channel(c)
    

def refresh_scsi_channel(channel):
    DEV_WAIT = 5
    util.SMlog("Refreshing channel %s" % channel)
    util.wait_for_path('/dev/disk/by-scsibus/*-%s*' % channel, DEV_WAIT)
    LUNs = glob.glob('/dev/disk/by-scsibus/*-%s*' % channel)
    try:
        rootdevs = util.dom0_disks()
    except:
        util.SMlog("Failed to query root disk, failing operation")
        return False
    
    # a) Find a LUN to issue a Query LUNs command
    li = []
    Query = False
    for lun in LUNs:
        try:
            hbtl = lun.split('-')[-1]
            h = hbtl.split(':')
            l=util.pread2(["/usr/bin/sg_luns","-q",lun]).split('\n')
            li = []
            for i in l:
                if len(i):
                    li.append(int(i[0:4], 16))
            util.SMlog("sg_luns query returned %s" % li)
            Query = True
            break
        except:
            pass
    if not Query:
        util.SMlog("Failed to detect or query LUN on Channel %s" % channel)
        return False

    # b) Remove stale LUNs
    current = glob.glob('/dev/disk/by-scsibus/*-%s:%s:%s*' % (h[0],h[1],h[2]))
    for cur in current:
        lunID = int(cur.split(':')[-1])
        newhbtl = ['',h[0],h[1],h[2],str(lunID)]
        if os.path.realpath(cur) in rootdevs:
            # Don't touch the rootdev
            if lunID in li: li.remove(lunID)
            continue
        
        # Check if LUN is stale, and remove it
        if not lunID in li:
            util.SMlog("Stale LUN detected. Removing HBTL: %s" % newhbtl)
            scsi_dev_ctrl(newhbtl,"remove")
            util.wait_for_nopath(cur, DEV_WAIT)
            continue
        else:
            li.remove(lunID)

        # Check if the device is still present
        if not os.path.exists(cur):
            continue
        
        # Query SCSIid, check it matches, if not, re-probe
        cur_SCSIid = os.path.basename(cur).split("-%s:%s:%s" % (h[0],h[1],h[2]))[0]
        real_SCSIid = getSCSIid(cur)
        if cur_SCSIid != real_SCSIid:
            util.SMlog("HBTL %s does not match, re-probing" % newhbtl)
            scsi_dev_ctrl(newhbtl,"remove")
            util.wait_for_nopath(cur, DEV_WAIT)
            scsi_dev_ctrl(newhbtl,"add")
            util.wait_for_path('/dev/disk/by-scsibus/%s-%s' % (real_SCSIid,hbtl), DEV_WAIT)
            pass

    # c) Probe for any LUNs that are not present in the system
    for l in li:
        newhbtl = ['',h[0],h[1],h[2],str(l)]
        newhbtlstr = "%s:%s:%s:%s" % (h[0],h[1],h[2],str(l))
        util.SMlog("Probing new HBTL: %s" % newhbtl)
        scsi_dev_ctrl(newhbtl,"add")
        util.wait_for_path('/dev/disk/by-scsibus/*-%s' % newhbtlstr, DEV_WAIT)

    return True


def refreshdev(pathlist):
    """
    Refresh block devices given a path list
    """
    for path in pathlist:
        dev = getdev(path)
        sysfs = os.path.join('/sys/block',dev,'device/rescan')
        if os.path.exists(sysfs):
            try:
                f = os.open(sysfs, os.O_WRONLY)
                os.write(f,'1')
                os.close(f)
            except:
                pass


def refresh_lun_size_by_SCSIid(SCSIid):
    """
    Refresh all devices for the SCSIid.
    Returns True if all known devices and the mapper device are up to date.
    """
    def get_primary_device(SCSIid):
        mapperdevice = os.path.join('/dev/mapper', SCSIid)
        if os.path.exists(mapperdevice):
            return mapperdevice
        else:
            devices = get_devices_by_SCSIid(SCSIid)
            if devices:
                return devices[0]
            else:
                return None

    def get_outdated_size_devices(currentcapacity, devices):
        devicesthatneedrefresh = []
        for device in devices:
            if getsize(device) != currentcapacity:
                devicesthatneedrefresh.append(device)
        return devicesthatneedrefresh

    def refresh_devices_if_needed(primarydevice, SCSIid, currentcapacity):
        devices = get_devices_by_SCSIid(SCSIid)
        if "/dev/mapper/" in primarydevice:
            devices = set(devices + mpath_cli.list_paths(SCSIid))
        devicesthatneedrefresh = get_outdated_size_devices(currentcapacity,
                                                           devices)
        if devicesthatneedrefresh:
            # timing out avoids waiting for min(dev_loss_tmo, fast_io_fail_tmo)
            # if one or multiple devices don't answer
            util.timeout_call(10, refreshdev, devicesthatneedrefresh)
            if get_outdated_size_devices(currentcapacity,
                                         devicesthatneedrefresh):
                # in this state we shouldn't force resizing the mapper dev
                raise util.SMException("Failed to get %s to agree on the "
                                       "current capacity."
                                       % devicesthatneedrefresh)

    def refresh_mapper_if_needed(primarydevice, SCSIid, currentcapacity):
        if "/dev/mapper/" in primarydevice \
           and get_outdated_size_devices(currentcapacity, [primarydevice]):
            mpath_cli.resize_map(SCSIid)
            if get_outdated_size_devices(currentcapacity, [primarydevice]):
                raise util.SMException("Failed to get the mapper dev to agree "
                                       "on the current capacity.")

    try:
        primarydevice = get_primary_device(SCSIid)
        if primarydevice:
            currentcapacity = sg_readcap(primarydevice)
            refresh_devices_if_needed(primarydevice, SCSIid, currentcapacity)
            refresh_mapper_if_needed(primarydevice, SCSIid, currentcapacity)
        else:
            util.SMlog("scsiutil.refresh_lun_size_by_SCSIid(%s) could not "
                       "find any devices for the SCSIid." % SCSIid)
        return True
    except:
        util.logException("Error in scsiutil.refresh_lun_size_by_SCSIid(%s)"
                          % SCSIid)
        return False


def refresh_lun_size_by_SCSIid_on_slaves(session, SCSIid):
    for slave in util.get_all_slaves(session):
        util.SMlog("Calling on-slave.refresh_lun_size_by_SCSIid(%s) on %s."
                   % (SCSIid, slave))
        resulttext = session.xenapi.host.call_plugin(
                                                  slave,
                                                  "on-slave",
                                                  "refresh_lun_size_by_SCSIid",
                                                  {'SCSIid': SCSIid })
        if "True" == resulttext:
            util.SMlog("Calling on-slave.refresh_lun_size_by_SCSIid(%s) on"
                       " %s succeeded." % (SCSIid, slave))
        else:
            message = ("Failed in on-slave.refresh_lun_size_by_SCSIid(%s) "
                       "on %s." % (SCSIid, slave))
            raise util.SMException("Slave %s failed in on-slave.refresh_lun_"
                                   "size_by_SCSIid(%s) " % (slave, SCSIid))


def remove_stale_luns(hostids, lunid, expectedPath, mpath):
    try:
        for hostid in hostids:            
            # get all LUNs of the format hostid:x:y:lunid
            luns = glob.glob('/dev/disk/by-scsibus/*-%s:*:*:%s' % (hostid, lunid))
            
            # try to get the scsiid for each of these luns
            for lun in luns:
                try:
                    getSCSIid(lun)
                    # if it works, we have a problem as this device should not
                    # be present and be valid on this system
                    util.SMlog("Warning! The lun %s should not be present and" \
                                    " be valid on this system." % lun)
                except:
                    # Now do the rest.
                    pass
                
                # get the HBTL
                basename = os.path.basename(lun)
                hbtl_list = basename.split(':')
                hbtl = basename.split('-')[1]
                
                # the first one in scsiid-hostid
                hbtl_list[0] = hbtl_list[0].split('-')[1]
                
                expectedPath = expectedPath + '*' + hbtl
                if not os.path.exists(expectedPath):
                    # wait for sometime and check if the expected path exists
                    # check if a rescan was done outside of this process
                    time.sleep(2)
                
                if os.path.exists(expectedPath):
                    # do not remove device, this might be dangerous
                    util.SMlog("Path %s appeared before checking for "\
                        "stale LUNs, ignore this LUN %s." % (expectedPath, lun))
                    continue                        
                    
                # remove the scsi device
                l = [os.path.realpath(lun), hbtl_list[0], hbtl_list[1], \
                     hbtl_list[2], hbtl_list[3]]
                scsi_dev_ctrl(l, 'remove')
                
                # if multipath is enabled, do a best effort cleanup
                if mpath:
                    try:
                        path = os.path.basename(os.path.realpath(lun))
                        mpath_cli.remove_path(path)
                    except Exception, e:
                        util.SMlog("Failed to remove path %s, ignoring "\
                            "exception as path may not be present." % path)
    except Exception, e:
        util.SMlog("Exception removing stale LUNs, new devices may not come"\
                   " up properly! Error: %s" % str(e))

def sg_readcap(device):
    device = os.path.join('/dev', getdev(device))
    readcapcommand = ['/usr/bin/sg_readcap', '-b', device]
    (rc,stdout,stderr) = util.doexec(readcapcommand)
    if rc == 6:
        # retry one time for "Capacity data has changed"
        (rc,stdout,stderr) = util.doexec(readcapcommand)
    if rc != 0:
        raise util.SMException("util.sg_readcap(%s) failed" % (device))
    (blockcount,blocksize) = stdout.split()
    return (int(blockcount, 0) * int(blocksize, 0))
