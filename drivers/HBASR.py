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
# HBASR: Hardware HBA LUN driver, e.g. Fibre Channel or SAS or
# hardware based iSCSI
#

import SR, VDI, SRCommand, ISCSISR
import devscan, scsiutil, util, LUNperVDI
import os, sys, re, time
import xs_errors
import xml.dom.minidom

CAPABILITIES = ["SR_PROBE","VDI_CREATE","VDI_DELETE","VDI_ATTACH",
                "VDI_DETACH", "VDI_INTRODUCE"]

CONFIGURATION = [ [ 'type', 'HBA type (optional) (e.g. FC, iSCSI, SAS etc..)' ] ]

DRIVER_INFO = {
    'name': 'HBA LUN-per-VDI driver',
    'description': 'SR plugin which represents LUNs as VDIs sourced by hardware HBA adapters, e.g. hardware-based iSCSI or FC support',
    'vendor': 'Citrix Systems Inc',
    'copyright': '(C) 2008 Citrix Systems Inc',
    'driver_version': '1.0',
    'required_api_version': '1.0',
    'capabilities': CAPABILITIES,
    'configuration': CONFIGURATION
    }

HBA_CLI      = "/opt/Citrix/StorageLink/bin/csl_hbascan"

class HBASR(SR.SR):
    """HBA storage repository"""
    def handles(type):
        if type == "hba":
            return True
        return False
    handles = staticmethod(handles)

    def load(self, sr_uuid):
        self.sr_vditype = 'phy'
        self.type = "any"
        if self.dconf.has_key('type') and self.dconf['type']:
            self.type = self.dconf['type']
        self.attached = False
        self.procname = ""
        self.devs = {}
            
    def _init_hbadict(self):
        if not hasattr(self, "hbas"):
            dict = devscan.adapters(filterstr=self.type)
            self.hbadict = dict['devs']
            self.hbas = dict['adt']
            if len(self.hbas):
                self.attached = True
                self.devs = scsiutil.cacheSCSIidentifiers()

    def _init_hba_hostname(self):
        """ get the HBA Host WWN information on this host function """

        fc_xml = self._probe_hba()
        nodewwnval = ''
        try:
            fcs = xml.dom.minidom.parseString(fc_xml)
            infos = fcs.getElementsByTagName("HBAInfo")
            for info in infos:
                nodewwn = info.getElementsByTagName("nodeWWN")
                nodewwnval = str(nodewwn[0].firstChild.nodeValue)
                break
        except:
            raise xs_errors.XenError('CSLGXMLParse', opterr='HBA Host WWN scanning failed')
        return nodewwnval

    def _init_hbas(self):
        """ get the HBA information on this host function """
        
        fc_xml = self._probe_hba()
        adt = {}   
        try:
            fcs = xml.dom.minidom.parseString(fc_xml)
            infos = fcs.getElementsByTagName("HBAInfo")
            # HBAInfo --> Port --> portWWN
            # HBAInfo --> Port --> deviceName
            for info in infos:
                ports = info.getElementsByTagName("Port")
                for port in ports:
                    portwwns = port.getElementsByTagName("portWWN")
                    devnames = port.getElementsByTagName("deviceName")
                    portval = str(portwwns[0].firstChild.nodeValue)
                    devpath = str(devnames[0].firstChild.nodeValue).split('/')[-1]
                    adt[devpath] = portval.split()[0]
        except:
            raise xs_errors.XenError('CSLGXMLParse', \
                                     opterr='HBA scanning failed')
        return adt

    def _probe_hba(self):
        try:
            # use sysfs tree to gather FC data

            dom = xml.dom.minidom.Document()
            hbalist = dom.createElement("HBAInfoList")
            dom.appendChild(hbalist)

            hostlist = util.listdir("/sys/class/fc_host")
            for host in hostlist: 

                hbainfo = dom.createElement("HBAInfo")
                hbalist.appendChild(hbainfo)

                cmd = ["cat", "/sys/class/fc_host/%s/symbolic_name" % host]
                sname = util.pread(cmd)[:-1]
                entry = dom.createElement("model")
                hbainfo.appendChild(entry)
                textnode = dom.createTextNode(sname)
                entry.appendChild(textnode)

                cmd = ["cat", "/sys/class/fc_host/%s/node_name" % host]
                nname = util.pread(cmd)[:-1]
                nname = util.make_WWN(nname)
                entry = dom.createElement("nodeWWN")
                hbainfo.appendChild(entry)
                # adjust nodename to look like expected string
                textnode = dom.createTextNode(nname)
                entry.appendChild(textnode)

                port = dom.createElement("Port")
                hbainfo.appendChild(port)

                cmd = ["cat", "/sys/class/fc_host/%s/port_name" % host]
                pname = util.pread(cmd)[:-1]
                pname = util.make_WWN(pname)
                entry = dom.createElement("portWWN")
                port.appendChild(entry)
                # adjust nodename to look like expected string
                textnode = dom.createTextNode(pname)
                entry.appendChild(textnode)

                cmd = ["cat", "/sys/class/fc_host/%s/port_state" % host]
                state = util.pread(cmd)[:-1]
                entry = dom.createElement("state")
                port.appendChild(entry)
                # adjust nodename to look like expected string
                textnode = dom.createTextNode(state)
                entry.appendChild(textnode)

                entry = dom.createElement("deviceName")
                port.appendChild(entry)
                # adjust nodename to look like expected string
                textnode = dom.createTextNode("/sys/class/scsi_host/%s" % host)
                entry.appendChild(textnode)

            return dom.toxml()
        except:
            raise xs_errors.XenError('CSLGXMLParse', \
                                     opterr='HBA probe failed')
    
    def attach(self, sr_uuid):
        self._mpathHandle()

    def detach(self, sr_uuid):
        if util._containsVDIinuse(self):
            return
        return

    def create(self, sr_uuid, size):
        # Check whether an SR already exists
        SRs = self.session.xenapi.SR.get_all_records()
        for sr in SRs:
            record = SRs[sr]
            sm_config = record["sm_config"]
            if sm_config.has_key('datatype') and \
               sm_config['datatype'] == 'HBA' and \
               sm_config['hbatype'] == self.type:
                raise xs_errors.XenError('SRInUse')
        self._init_hbadict()
        if not self.attached:
            raise xs_errors.XenError('InvalidDev', \
                      opterr=('No such HBA Device detected [%s]') % self.type)

        if self._loadvdis() > 0:
            scanrecord = SR.ScanRecord(self)
            scanrecord.synchronise()
        try:
            self.detach(sr_uuid)
        except:
            pass
        self.sm_config = self.session.xenapi.SR.get_sm_config(self.sr_ref)
        self.sm_config['disktype'] = 'Raw'
        self.sm_config['datatype'] = 'HBA'
        self.sm_config['hbatype'] = self.type
        self.sm_config['multipathable'] = 'true'
        self.session.xenapi.SR.set_sm_config(self.sr_ref, self.sm_config)

    def delete(self, sr_uuid):
        self.detach(sr_uuid)
        return

    def probe(self):
        self._init_hbadict()
        self.attach("")
        SRs = self.session.xenapi.SR.get_all_records()
        Recs = {}
        for sr in SRs:
            record = SRs[sr]
            sm_config = record["sm_config"]
            if sm_config.has_key('datatype') and \
               sm_config['datatype'] == 'HBA':
                Recs[record["uuid"]] = sm_config
        return self.srlist_toxml(Recs)
    
    def scan(self, sr_uuid):
        self._init_hbadict()
        if not self.passthrough:
            if not self.attached:
                raise xs_errors.XenError('SRUnavailable')
            # HBA adapter scan already forced a bus rescan
            # Sleep for 2 seconds to allow devices to settle
            time.sleep(2)
            self._loadvdis()
            self.physical_utilisation = self.physical_size
            for uuid, vdi in self.vdis.iteritems():
                if vdi.managed:
                    self.physical_utilisation += vdi.size
            self.virtual_allocation = self.physical_utilisation
        return super(HBASR, self).scan(sr_uuid)

    def print_devs(self):
        self.attach("")
        self._init_hbadict()
        return devscan.scan(self)

    # This function returns a dictionary of HBA attached LUNs
    def _loadvdis(self):
        if self.vdis:
            return

        self._init_hbadict()
        count = 0
        for key in self.hbadict.iterkeys():
            vdi_path = os.path.join("/dev",key)
	    if not self.devs.has_key(vdi_path):
		continue
            uuid = scsiutil.gen_uuid_from_string(scsiutil.getuniqueserial(vdi_path))
            obj = self.vdi(uuid)
            path = self.mpathmodule.path(scsiutil.getSCSIid(vdi_path))
	    ids = self.devs[vdi_path]
            obj._query(vdi_path, ids[4])
            self.vdis[uuid] = obj
            self.physical_size += obj.size
            count += 1
        return count

    def _getLUNbySMconfig(self, sm_config):
        raise xs_errors.XenError('VDIUnavailable')
        
    def vdi(self, uuid):
        return LUNperVDI.RAWVDI(self, uuid)

    def srlist_toxml(self, SRs):
        dom = xml.dom.minidom.Document()
        element = dom.createElement("SRlist")
        dom.appendChild(element)

        for val in SRs:
            record = SRs[val]
            entry = dom.createElement('SR')
            element.appendChild(entry)

            subentry = dom.createElement("UUID")
            entry.appendChild(subentry)
            textnode = dom.createTextNode(val)
            subentry.appendChild(textnode)
        return dom.toprettyxml()

if __name__ == '__main__':
    SRCommand.run(HBASR, DRIVER_INFO)
else:
    SR.registerSR(HBASR)
