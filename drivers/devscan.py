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

import sys, os, re
import scsiutil, util
import xml.dom.minidom
import xs_errors, time
import glob

DEVPATH='/dev/disk/by-id'
DMDEVPATH='/dev/mapper'
SYSFS_PATH1='/sys/class/scsi_host'
SYSFS_PATH2='/sys/class/scsi_disk'
SYSFS_PATH3='/sys/class/fc_transport'

MODULE_INFO = {
    'brocade': 'Brocade HBA Driver',
    'cxgb3': 'Chelsio T3 HBA Driver',
    'cxgb4': 'Chelsio T4 HBA Driver',
    'csiostor': 'Chelsio T4/T5 FCoE Driver',
    'qlogic': 'QLogic HBA Driver',
    'lpfc': 'Emulex Device Driver for Fibre Channel HBAs',
    'mptfc': 'LSI Logic Fusion MPT Fibre Channel Driver',
    'mptsas': 'LSI Logic Fusion MPT SAS Adapter Driver',
    'mpt2sas': 'LSI Logic Fusion MPT 6GB SAS Adapter Driver',
    'megaraid_sas': 'MegaRAID driver for SAS based RAID controllers',
    'palo' : 'Cisco Palo FCoE Adapter driver',
    'ethdrv' : 'Coraid ATA over Ethernet driver',
    'xsvhba': 'Xsigo Systems Virtual HBA Driver',
    'mpp': 'RDAC Multipath Handler, manages DELL devices from other adapters'
    }

def getManufacturer(s):
    for e in MODULE_INFO.iterkeys():
        regex = re.compile("^%s" % e)
        if regex.search(s, 0):
            return MODULE_INFO[e]
    return "Unknown"

def gen_QLadt():
    host = []
    arr = glob.glob('/sys/bus/pci/drivers/qla*/*/*host*') +\
          glob.glob('/sys/bus/pci/drivers/qlisa/*/*host*')
    # output may be in the form "host#" or "scsi_host:host#"
    for val in arr:
        node = val.split('/')[-1]
        entry = re.sub("^.*:","",node)
        if entry not in host:
            host.append(entry)
    return host

def gen_brocadt():
    host = []
    arr = glob.glob('/sys/bus/pci/drivers/bfa/*/host*')
    for val in arr:
        host.append(val.split('/')[-1])
    return host

def gen_palo():
    host = []
    arr = glob.glob('/sys/bus/pci/drivers/fnic/*/host*')
    for val in arr:
        host.append(val.split('/')[-1])
    return host

def adapters(filterstr="any"):
    dict = {}
    devs = {}
    adt = {}
    QL = gen_QLadt()
    BC = gen_brocadt()
    CS = gen_palo()
    for a in os.listdir(SYSFS_PATH1):
        if a in QL:
            proc = "qlogic"
        elif a in BC:
            proc = "brocade"
        elif a in CS:
            proc = "palo"
        else:
            proc = match_hbadevs(a, filterstr)
            if not proc:
                continue
        adt[a] = proc
        id = a.replace("host","")
        scsiutil.rescan([id])
        emulex = False
        paths = []
        if proc == "lpfc":
            emulex = True
            paths.append(SYSFS_PATH3)
        else:
            for p in [os.path.join(SYSFS_PATH1,a,"device","session*"),os.path.join(SYSFS_PATH1,a,"device"),\
                          os.path.join(SYSFS_PATH2,"%s:*"%id)]:
                paths += glob.glob(p)
        if not len(paths):
            continue
        for path in paths:
            for i in filter(match_targets,os.listdir(path)):
                tgt = i.replace('target','')
                if emulex:
                    sysfs = os.path.join(SYSFS_PATH3,i,"device")
                else:
                    sysfs = SYSFS_PATH2
                for lun in os.listdir(sysfs):
                    if not match_LUNs(lun,tgt):
                        continue
                    if emulex:
                        dir = os.path.join(sysfs,lun)
                    else:
                        dir = os.path.join(sysfs,lun,"device")
                    (dev, entry) = _extract_dev(dir, proc, id, lun)
                    devs[dev] = entry
            # for new qlogic sysfs layout (rport under device, then target)
            for i in filter(match_rport,os.listdir(path)):
                newpath = os.path.join(path, i)
                for j in filter(match_targets,os.listdir(newpath)):
                    tgt = j.replace('target','')
                    sysfs = SYSFS_PATH2
                    for lun in os.listdir(sysfs):
                        if not match_LUNs(lun,tgt):
                            continue
                        dir = os.path.join(sysfs,lun,"device")
                        (dev, entry) = _extract_dev(dir, proc, id, lun)
                        devs[dev] = entry

            # for new mptsas sysfs entries, check for phy* node
            for i in filter(match_phy,os.listdir(path)):
                (target,lunid) = i.replace('phy-','').split(':')
                tgt = "%s:0:0:%s" % (target,lunid)
                sysfs = SYSFS_PATH2
                for lun in os.listdir(sysfs):
                    if not match_LUNs(lun,tgt):
                        continue
                    dir = os.path.join(sysfs,lun,"device")
                    (dev, entry) = _extract_dev(dir, proc, id, lun)
                    devs[dev] = entry
            if path.startswith(SYSFS_PATH2):
                os.path.join(path,"device","block:*")
                dev = _extract_dev_name(os.path.join(path, 'device'))
                if devs.has_key(dev):
                    continue
                hbtl = os.path.basename(path)
                (h,b,t,l) = hbtl.split(':')
                entry = {'procname':proc, 'host':id, 'target':l}
                devs[dev] = entry

    dict['devs'] = devs
    dict['adt'] = adt
    return dict
            
def _getField(s):
    f = open(s, 'r')
    line = f.readline()[:-1]
    f.close()
    return line

def _parseHostId(str):
    id = str.split()
    val = "%s:%s:%s" % (id[1],id[3],id[5])
    return val.replace(',','')

def _genMPPHBA(id):
    devs = scsiutil.cacheSCSIidentifiers()
    mppdict = {}
    for dev in devs:
        item = devs[dev]
        if item[1] == id:
            arr = scsiutil._genArrayIdentifier(dev)
            if not len(arr):
                continue
            try:
                cmd = ['/usr/sbin/mppUtil', '-a']
                for line in util.doexec(cmd)[1].split('\n'):
                    if line.find(arr) != -1:
                        rec = line.split()[0]
                        cmd2 = ['/usr/sbin/mppUtil', '-g',rec]
                        li = []
                        for newline in util.doexec(cmd2)[1].split('\n'):
                            if newline.find('hostId') != -1:
                                li.append(_parseHostId(newline))
                        mppdict[dev.split('/')[-1]] = li
            except:
                continue
    return mppdict

def match_hbadevs(s, filterstr):
    regex = re.compile("^host[0-9]")
    if not regex.search(s, 0):
        return ""
    try:
        if os.path.exists(os.path.join(SYSFS_PATH1,s,"lpfc_fcp_class")):
            pname = "lpfc"
        else:
            filename = os.path.join(SYSFS_PATH1,s,"proc_name")
            pname = _getField(filename)
    except:
        return ""

    if filterstr == "any":
        for e in MODULE_INFO.iterkeys():
            regex = re.compile("^%s" % e)
            if regex.search(pname, 0):
                return pname
    else:
        regex = re.compile("^%s" % filterstr)
        if regex.search(pname, 0):
            return pname
    return ""

def match_rport(s):
    regex = re.compile("^rport-*")
    return regex.search(s, 0)

def match_targets(s):
    regex = re.compile("^target[0-9]")
    return regex.search(s, 0)

def match_phy(s):
    regex = re.compile("^phy-*")
    return regex.search(s, 0)

def match_LUNs(s, prefix):
    regex = re.compile("^%s" % prefix)
    return regex.search(s, 0)    

def match_dev(s):
    regex = re.compile("^block:")
    return regex.search(s, 0)

def _extract_dev_name(device_dir):
    """Returns the name of the block device from sysfs e.g. 'sda'"""
    kernel_version = os.uname()[2]

    if kernel_version.startswith('2.6'):
        # sub-directory of form block:sdx/
        dev = filter(match_dev, os.listdir(device_dir))[0]
        # remove 'block:' from entry and return
        return dev.lstrip('block:')
    elif kernel_version.startswith('3.'):
        # directory for device name lives inside block directory e.g. block/sdx
        dev = glob.glob(os.path.join(device_dir, 'block/*'))[0]
        # prune path to extract the device name
        return os.path.basename(dev)
    else:
        msg = 'Kernel version detected: %s' % kernel_version
        raise xs_errors.XenError('UnsupportedKernel', msg)

def _extract_dev(device_dir, procname, host, target):
    """Returns device name and creates dictionary entry for it"""
    dev = _extract_dev_name(device_dir)
    entry = {}
    entry['procname'] = procname
    entry['host'] = host
    entry['target'] = target
    return (dev, entry)

def scan(srobj):
    systemrootID = util.getrootdevID()
    hbadict = srobj.hbadict
    hbas = srobj.hbas
    dom = xml.dom.minidom.Document()
    e = dom.createElement("Devlist")
    dom.appendChild(e)

    if not os.path.exists(DEVPATH):
        return dom.toprettyxml()
    
    devs = srobj.devs
    vdis = {}

    for key in hbadict:
        hba = hbadict[key]
        path = os.path.join("/dev",key)
        realpath = path

        obj = srobj.vdi("")
        try:
            obj._query(realpath, devs[realpath][4])
        except:
            continue
        
        # Test for root dev or existing PBD
        if len(obj.SCSIid) and len(systemrootID) and util.match_scsiID(obj.SCSIid, systemrootID):
            util.SMlog("Ignoring root device %s" % realpath)
            continue
        elif util.test_SCSIid(srobj.session, None, obj.SCSIid):
            util.SMlog("SCSIid in use, ignoring (%s)" % obj.SCSIid)
            continue
        elif not devs.has_key(realpath):
            continue
        
        ids = devs[realpath]
        obj.adapter = ids[1]
        obj.channel = ids[2]
        obj.id = ids[3]
        obj.lun = ids[4]
        obj.hba = hba['procname']
        obj.numpaths = 1
        if vdis.has_key(obj.SCSIid):
            vdis[obj.SCSIid].numpaths += 1
            vdis[obj.SCSIid].path += " [%s]" % key
        elif obj.hba == 'mpp':
            mppdict = _genMPPHBA(obj.adapter)
            if mppdict.has_key(key):
                item = mppdict[key]
                adapters = ''
                for i in item:
                    if len(adapters):
                        adapters += ', '
                        obj.numpaths += 1
                    adapters += i
                if len(adapters):
                    obj.mpp = adapters
            vdis[obj.SCSIid] = obj
        else:    
            vdis[obj.SCSIid] = obj
                    
    for key in vdis:
        obj = vdis[key]
        d = dom.createElement("BlockDevice")
        e.appendChild(d)

        for attr in ['path','numpaths','SCSIid','vendor','serial','size','adapter','channel','id','lun','hba','mpp']:
            try:
                aval = getattr(obj, attr)
            except AttributeError:
                if attr in ['mpp']:
                    continue
                raise xs_errors.XenError('InvalidArg', \
                      opterr='Missing required field [%s]' % attr)
            entry = dom.createElement(attr)
            d.appendChild(entry)
            textnode = dom.createTextNode(str(aval))
            entry.appendChild(textnode)

    for key in hbas.iterkeys():
        a = dom.createElement("Adapter")
        e.appendChild(a)
        entry = dom.createElement('host')
        a.appendChild(entry)
        textnode = dom.createTextNode(key)
        entry.appendChild(textnode)

        entry = dom.createElement('name')
        a.appendChild(entry)
        textnode = dom.createTextNode(hbas[key])
        entry.appendChild(textnode)

        entry = dom.createElement('manufacturer')
        a.appendChild(entry)
        textnode = dom.createTextNode(getManufacturer(hbas[key]))
        entry.appendChild(textnode)

        id = key.replace("host","")
        entry = dom.createElement('id')
        a.appendChild(entry)
        textnode = dom.createTextNode(id)
        entry.appendChild(textnode)       
        
    return dom.toprettyxml()

def check_iscsi(adapter):
    ret = False
    str = "host%s" % adapter
    try:
        filename = os.path.join('/sys/class/scsi_host',str,'proc_name')
        f = open(filename, 'r')
        if f.readline().find("iscsi_tcp") != -1:
            ret = True
    except:
        pass
    return ret            

def match_nonpartitions(s):
    regex = re.compile("-part[0-9]")
    if not regex.search(s, 0):
        return True
