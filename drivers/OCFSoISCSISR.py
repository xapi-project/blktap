#!/usr/bin/python
# Copyright (C) 2008-2013 Citrix Ltd.
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
# OCFSoISCSISR: OCFS over ISCSI software initiator SR driver
#

import SR, VDI, OCFSSR, ISCSISR, SRCommand, util, scsiutil
import statvfs
import os, sys
import xs_errors
import xmlrpclib
import mpath_cli, iscsilib
import glob, copy
import mpp_luncheck
import scsiutil
import xml.dom.minidom

CAPABILITIES = ["SR_PROBE", "SR_UPDATE", "SR_METADATA", "VDI_CREATE",
                "VDI_DELETE", "VDI_ATTACH", "VDI_DETACH",
                "VDI_GENERATE_CONFIG", "VDI_CLONE", "VDI_SNAPSHOT",
                "VDI_RESIZE", "ATOMIC_PAUSE", "VDI_UPDATE"]

CONFIGURATION = [ [ 'SCSIid', 'The scsi_id of the destination LUN' ], \
                  [ 'target', 'IP address or hostname of the iSCSI target' ], \
                  [ 'targetIQN', 'The IQN of the target LUN group to be attached' ], \
                  [ 'chapuser', 'The username to be used during CHAP authentication' ], \
                  [ 'chappassword', 'The password to be used during CHAP authentication' ], \
                  [ 'incoming_chapuser', 'The incoming username to be used during bi-directional CHAP authentication (optional)' ], \
                  [ 'incoming_chappassword', 'The incoming password to be used during bi-directional CHAP authentication (optional)' ], \
                  [ 'port', 'The network port number on which to query the target' ], \
                  [ 'multihomed', 'Enable multi-homing to this target, true or false (optional, defaults to same value as host.other_config:multipathing)' ], \
                  [ 'usediscoverynumber', 'The specific iscsi record index to use. (optional)' ], \
                  [ 'allocation', 'Valid values are thick or thin (optional, defaults to thick)'] ]

DRIVER_INFO = {
    'name': 'OCFS over iSCSI',
    'description': 'SR plugin which represents disks as Logical Volumes within a Volume Group created on an iSCSI LUN',
    'vendor': 'Citrix Systems Inc',
    'copyright': '(C) 2008-2013 Citrix Systems Inc',
    'driver_version': '1.0',
    'required_api_version': '1.0',
    'capabilities': CAPABILITIES,
    'configuration': CONFIGURATION
    }

class OCFSoISCSISR(OCFSSR.OCFSSR):
    """OCFS over ISCSI storage repository"""
    def handles(type):
        if __name__ == '__main__': 
            name = sys.argv[0]
        else:
            name = __name__
        if name.endswith("OCFSoISCSISR"):
            return type == "ocfsoiscsi"

        return False
    handles = staticmethod(handles)

    def load(self, sr_uuid):
        if not sr_uuid:
            # This is a probe call, generate a temp sr_uuid
            sr_uuid = util.gen_uuid()

        driver = SR.driver('iscsi')
        if self.original_srcmd.dconf.has_key('target'):
            self.original_srcmd.dconf['targetlist'] = self.original_srcmd.dconf['target']
        iscsi = driver(self.original_srcmd, sr_uuid)
        self.iscsiSRs = []
        self.iscsiSRs.append(iscsi)
        
        if self.dconf['target'].find(',') == 0 or self.dconf['targetIQN'] == "*":
            # Instantiate multiple sessions
            self.iscsiSRs = []
            if self.dconf['targetIQN'] == "*":
                IQN = "any"
            else:
                IQN = self.dconf['targetIQN']
            dict = {}
            IQNstring = ""
            IQNs = []
            try:
                if self.dconf.has_key('multiSession'):
                    IQNs = self.dconf['multiSession'].split("|")
                    for IQN in IQNs:
                        if IQN:
                            dict[IQN] = ""
                        else:
                            try:
                                IQNs.remove(IQN)
                            except:
                                # Exceptions are not expected but just in case
                                pass
                    # Order in multiSession must be preserved. It is important for dual-controllers.
                    # IQNstring cannot be built with a dictionary iteration because of this
                    IQNstring = self.dconf['multiSession']
                else:
                    for tgt in self.dconf['target'].split(','):
                        try:
                            tgt_ip = util._convertDNS(tgt)
                        except:
                            raise xs_errors.XenError('DNSError')
                        iscsilib.ensure_daemon_running_ok(iscsi.localIQN)
                        map = iscsilib.discovery(tgt_ip,iscsi.port,iscsi.chapuser,iscsi.chappassword,targetIQN=IQN)
                        util.SMlog("Discovery for IP %s returned %s" % (tgt,map))
                        for i in range(0,len(map)):
                            (portal,tpgt,iqn) = map[i]
                            (ipaddr,port) = portal.split(',')[0].split(':')
                            try:
                                util._testHost(ipaddr, long(port), 'ISCSITarget')
                            except:
                                util.SMlog("Target Not reachable: (%s:%s)" % (ipaddr, port))
                                continue
                            key = "%s,%s,%s" % (ipaddr,port,iqn)
                            dict[key] = ""
                # Again, do not mess up with IQNs order. Dual controllers will benefit from that
                if IQNstring == "":
                    # Compose the IQNstring first
                    for key in dict.iterkeys(): IQNstring += "%s|" % key
                    # Reinitialize and store iterator
                    key_iterator = dict.iterkeys()
                else:
                    key_iterator = IQNs

                # Now load the individual iSCSI base classes
                for key in key_iterator:
                    (ipaddr,port,iqn) = key.split(',')
                    srcmd_copy = copy.deepcopy(self.original_srcmd)
                    srcmd_copy.dconf['target'] = ipaddr
                    srcmd_copy.dconf['targetIQN'] = iqn
                    srcmd_copy.dconf['multiSession'] = IQNstring
                    util.SMlog("Setting targetlist: %s" % srcmd_copy.dconf['targetlist'])
                    self.iscsiSRs.append(driver(srcmd_copy, sr_uuid))
                pbd = util.find_my_pbd(self.session, self.host_ref, self.sr_ref)
                if pbd <> None and not self.dconf.has_key('multiSession'):
                    dconf = self.session.xenapi.PBD.get_device_config(pbd)
                    dconf['multiSession'] = IQNstring
                    self.session.xenapi.PBD.set_device_config(pbd, dconf)
            except:
                util.logException("OCFSoISCSISR.load")
        self.iscsi = self.iscsiSRs[0]

        # Be extremely careful not to throw exceptions here since this function
        # is the main one used by all operations including probing and creating
        pbd = None
        try:
            pbd = util.find_my_pbd(self.session, self.host_ref, self.sr_ref)
        except:
            pass

        # Apart from the upgrade case, user must specify a SCSIid
        if not self.dconf.has_key('SCSIid'):
            # Dual controller issue
            self.LUNs = {}  # Dict for LUNs from all the iscsi objects
            for ii in range(0, len(self.iscsiSRs)):
                self.iscsi = self.iscsiSRs[ii]
                self._LUNprint(sr_uuid)
                for key in self.iscsi.LUNs:
                    self.LUNs[key] = self.iscsi.LUNs[key]
            self.print_LUNs_XML()
            self.iscsi = self.iscsiSRs[0] # back to original value
            raise xs_errors.XenError('ConfigSCSIid')

        self.SCSIid = self.dconf['SCSIid']

        # This block checks if the first iscsi target contains the right SCSIid.
        # If not it scans the other iscsi targets because chances are that more
        # than one controller is present
        dev_match = False
        forced_login = False
        # No need to check if only one iscsi target is present
        if len(self.iscsiSRs) == 1:
            pass
        else:
            for iii in range(0, len(self.iscsiSRs)):
                # Check we didn't leave any iscsi session open
                # If exceptions happened before, the cleanup function has worked on the right target.
                if forced_login == True:
                    try:
                        iscsilib.ensure_daemon_running_ok(self.iscsi.localIQN)
                        iscsilib.logout(self.iscsi.target, self.iscsi.targetIQN)
                        forced_login = False
                    except:
                        raise xs_errors.XenError('ISCSILogout')
                self.iscsi = self.iscsiSRs[iii]
                util.SMlog("path %s" %self.iscsi.path)
                util.SMlog("iscsci data: targetIQN %s, portal %s" % (self.iscsi.targetIQN, self.iscsi.target))
                iscsilib.ensure_daemon_running_ok(self.iscsi.localIQN)
                if not iscsilib._checkTGT(self.iscsi.targetIQN):
                    # Ensure iscsi db has been populated
                    map = iscsilib.discovery(self.iscsi.target, self.iscsi.port, self.iscsi.chapuser, \
                                             self.iscsi.chappassword, targetIQN=self.iscsi.targetIQN)
                    if len(map) == 0:
                        raise xs_errors.XenError('ISCSIDiscovery',
                                                  opterr='check target settings')
                    try:
                        iscsilib.login(self.iscsi.target, self.iscsi.targetIQN, self.iscsi.chapuser, \
                                       self.iscsi.chappassword, self.iscsi.incoming_chapuser, \
                                       self.iscsi.incoming_chappassword)
                    except:
                        raise xs_errors.XenError('ISCSILogin')
                    forced_login = True
                # A session should be active.
                if not util.wait_for_path(self.iscsi.path, ISCSISR.MAX_TIMEOUT):
                    util.SMlog("%s has no associated LUNs" % self.iscsi.targetIQN)
                    continue
                scsiid_path = "/dev/disk/by-id/scsi-" + self.SCSIid
                if not util.wait_for_path(scsiid_path, ISCSISR.MAX_TIMEOUT):
                    util.SMlog("%s not found" %scsiid_path)
                    continue
                for file in filter(self.iscsi.match_lun, util.listdir(self.iscsi.path)):
                    lun_path = os.path.join(self.iscsi.path,file)
                    lun_dev = scsiutil.getdev(lun_path)
                    lun_scsiid = scsiutil.getSCSIid(lun_dev)
                    util.SMlog("dev from lun %s %s" %(lun_dev, lun_scsiid))
                    if lun_scsiid == self.SCSIid:
                        util.SMlog("lun match in %s" %self.iscsi.path)
                        dev_match = True
                        break
                if dev_match:
                    if iii == 0:
                        break
                    util.SMlog("IQN reordering needed")
                    new_iscsiSRs = []
                    IQNs = {}
                    IQNstring = ""
                    # iscsiSRs can be seen as a circular buffer: the head now is the matching one
                    for kkk in range(iii, len(self.iscsiSRs)) + range(0, iii):
                        new_iscsiSRs.append(self.iscsiSRs[kkk])
                        ipaddr = self.iscsiSRs[kkk].target
                        port = self.iscsiSRs[kkk].port
                        iqn = self.iscsiSRs[kkk].targetIQN
                        key = "%s,%s,%s" % (ipaddr,port,iqn)
                        # The final string must preserve the order without repetition
                        if not IQNs.has_key(key):
                            IQNs[key] = ""
                            IQNstring += "%s|" % key
                    util.SMlog("IQNstring is now %s" %IQNstring)
                    self.iscsiSRs = new_iscsiSRs
                    util.SMlog("iqn %s is leading now" %self.iscsiSRs[0].targetIQN)
                    # Updating pbd entry, if any
                    try:
                        pbd = util.find_my_pbd(self.session, self.host_ref, self.sr_ref)
                        if pbd <> None and self.dconf.has_key('multiSession'):
                            util.SMlog("Updating multiSession in PBD")
                            dconf = self.session.xenapi.PBD.get_device_config(pbd)
                            dconf['multiSession'] = IQNstring
                            self.session.xenapi.PBD.set_device_config(pbd, dconf)
                    except:
                        pass
                    break

        self._pathrefresh(OCFSoISCSISR)

        OCFSSR.OCFSSR.load(self, sr_uuid)

    def print_LUNs_XML(self):
        dom = xml.dom.minidom.Document()
        element = dom.createElement("iscsi-target")
        dom.appendChild(element)
        # Omit the scsi-id used by iSL
        isl_scsiids = util.get_isl_scsiids(self.session)
        for uuid in self.LUNs:
            val = self.LUNs[uuid]
            if getattr(val,'SCSIid') in isl_scsiids:
                continue
            entry = dom.createElement('LUN')
            element.appendChild(entry)

            for attr in ('vendor', 'serial', 'LUNid', \
                         'size', 'SCSIid'):
                try:
                    aval = getattr(val, attr)
                except AttributeError:
                    continue

                if aval:
                    subentry = dom.createElement(attr)
                    entry.appendChild(subentry)
                    textnode = dom.createTextNode(str(aval))
                    subentry.appendChild(textnode)

        print >>sys.stderr,dom.toprettyxml()

    def _getSCSIid_from_LUN(self, sr_uuid):
        was_attached = True
        self.iscsi.attach(sr_uuid)
        dev = self.dconf['LUNid'].split(',')
        if len(dev) > 1:
            raise xs_errors.XenError('OCFSOneLUN')
        path = os.path.join(self.iscsi.path,"LUN%s" % dev[0])
        if not util.wait_for_path(path, ISCSISR.MAX_TIMEOUT):
            util.SMlog("Unable to detect LUN attached to host [%s]" % path)
        try:
            SCSIid = scsiutil.getSCSIid(path)
        except:
            raise xs_errors.XenError('InvalidDev')
        self.iscsi.detach(sr_uuid)
        return SCSIid

    def _LUNprint(self, sr_uuid):
        if self.iscsi.attached:
            # Force a rescan on the bus.
            self.iscsi.refresh()
#            time.sleep(5)
        # Now call attach (handles the refcounting + session activa)
        self.iscsi.attach(sr_uuid)

        util.SMlog("LUNprint: waiting for path: %s" % self.iscsi.path)
        if util.wait_for_path("%s/LUN*" % self.iscsi.path, ISCSISR.MAX_TIMEOUT):
            try:
                adapter=self.iscsi.adapter[self.iscsi.address]
                util.SMlog("adapter=%s" % adapter)

                # find a scsi device on which to issue a report luns command:
                devs=glob.glob("%s/LUN*" % self.iscsi.path)
                sgdevs = []
                for i in devs:
                    sgdevs.append(int(i.split("LUN")[1]))
                sgdevs.sort()
                sgdev = "%s/LUN%d" % (self.iscsi.path,sgdevs[0])                

                # issue a report luns:
                luns=util.pread2(["/usr/bin/sg_luns","-q",sgdev]).split('\n')
                nluns=len(luns)-1 # remove the line relating to the final \n
                # check if the LUNs are MPP-RDAC Luns
                scsi_id = scsiutil.getSCSIid(sgdev)
                mpp_lun = False
                if (mpp_luncheck.is_RdacLun(scsi_id)):
                    mpp_lun = True
                    link=glob.glob('/dev/disk/by-scsibus/%s-*' % scsi_id)
                    mpp_adapter = link[0].split('/')[-1].split('-')[-1].split(':')[0]

                # make sure we've got that many sg devices present
                for i in range(0,30): 
                    luns=scsiutil._dosgscan()
                    sgdevs=filter(lambda r: r[1]==adapter, luns)
                    if mpp_lun:
                        sgdevs.extend(filter(lambda r: r[1]==mpp_adapter, luns))
                    if len(sgdevs)>=nluns:
                        util.SMlog("Got all %d sg devices" % nluns)
                        break
                    else:
                        util.SMlog("Got %d sg devices - expecting %d" % (len(sgdevs),nluns))
                        time.sleep(1)

                util.pread2(["/sbin/udevsettle"])
            except:
                pass # Make sure we don't break the probe...

        self.iscsi.print_LUNs()
        self.iscsi.detach(sr_uuid)        
        
    def create(self, sr_uuid, size):
        # Check SCSIid not already in use by other PBDs
        if util.test_SCSIid(self.session, sr_uuid, self.SCSIid):
            raise xs_errors.XenError('SRInUse')

        self.iscsi.attach(sr_uuid)
        try:
            if not self.iscsi._attach_LUN_bySCSIid(self.SCSIid):
                # UPGRADE FROM GEORGE: take care of ill-formed SCSIid
                upgraded = False
                matchSCSIid = False
                for file in filter(self.iscsi.match_lun, util.listdir(self.iscsi.path)):
                    path = os.path.join(self.iscsi.path,file)
                    if not util.wait_for_path(path, ISCSISR.MAX_TIMEOUT):
                        util.SMlog("Unable to detect LUN attached to host [%s]" % path)
                        continue
                    try:
                        SCSIid = scsiutil.getSCSIid(path)
                    except:
                        continue
                    try:
                        matchSCSIid = scsiutil.compareSCSIid_2_6_18(self.SCSIid, path)
                    except:
                        continue
                    if (matchSCSIid):
                        util.SMlog("Performing upgrade from George")
                        try:
                            pbd = util.find_my_pbd(self.session, self.host_ref, self.sr_ref)
                            device_config = self.session.xenapi.PBD.get_device_config(pbd)
                            device_config['SCSIid'] = SCSIid
                            self.session.xenapi.PBD.set_device_config(pbd, device_config)

                            self.dconf['SCSIid'] = SCSIid            
                            self.SCSIid = self.dconf['SCSIid']
                        except:
                            continue
                        if not self.iscsi._attach_LUN_bySCSIid(self.SCSIid):
                            raise xs_errors.XenError('InvalidDev')
                        else:
                            upgraded = True
                            break
                    else:
                        util.SMlog("Not a matching LUN, skip ... scsi_id is: %s" % SCSIid)
                        continue
                if not upgraded:
                    raise xs_errors.XenError('InvalidDev')
            self._pathrefresh(OCFSoISCSISR)
            OCFSSR.OCFSSR.create(self, sr_uuid, size)
        except Exception, inst:
            self.iscsi.detach(sr_uuid)
            raise xs_errors.XenError("SRUnavailable", opterr=inst)
        self.iscsi.detach(sr_uuid)

    def delete(self, sr_uuid):
        OCFSSR.OCFSSR.delete(self, sr_uuid)
        for i in self.iscsiSRs:
            i.detach(sr_uuid)

    def attach(self, sr_uuid):
        try:
            connected = False
            for i in self.iscsiSRs:
                try:
                    i.attach(sr_uuid)
                except SR.SROSError, inst:
                    # Some iscsi objects can fail login but not all. Storing exception
                    if inst.errno == 141:
                        util.SMlog("Connection failed for target %s, continuing.." %i.target)
                        stored_exception = inst
                        continue
                    else:
                        raise
                else:
                    connected = True
                # Check if at least on iscsi succeeded
                if not connected:
                    raise stored_exception

                if not i._attach_LUN_bySCSIid(self.SCSIid):
                    raise xs_errors.XenError('InvalidDev')
            if self.dconf.has_key('multiSession'):
                # Force a manual bus refresh
                for a in self.iscsi.adapter:
                    scsiutil.rescan([self.iscsi.adapter[a]])
            self._pathrefresh(OCFSoISCSISR)
            OCFSSR.OCFSSR.attach(self, sr_uuid)
        except Exception, inst:
            for i in self.iscsiSRs:
                i.detach(sr_uuid)
            raise xs_errors.XenError("SRUnavailable", opterr=inst)
        self._setMultipathableFlag(SCSIid=self.SCSIid)
        
    def detach(self, sr_uuid):
        OCFSSR.OCFSSR.detach(self, sr_uuid)
        for i in self.iscsiSRs:
            i.detach(sr_uuid)

    def probe(self):
        self.uuid = util.gen_uuid()

# When multipathing is enabled, since we don't refcount the multipath maps,
# we should not attempt to do the iscsi.attach/detach when the map is already present,
# as this will remove it (which may well be in use).
        if self.mpath == 'true' and self.dconf.has_key('SCSIid'):
            maps = []
            mpp_lun = False
            try:
                if (mpp_luncheck.is_RdacLun(self.dconf['SCSIid'])):
                    mpp_lun = True
                    link=glob.glob('/dev/disk/mpInuse/%s-*' % self.dconf['SCSIid'])
                else:
                    maps = mpath_cli.list_maps()
            except:
                pass

            if (mpp_lun):
                if (len(link)):
                    raise xs_errors.XenError('SRInUse')
            else:
                if self.dconf['SCSIid'] in maps:
                    raise xs_errors.XenError('SRInUse')

        self.iscsi.attach(self.uuid)
        if not self.iscsi._attach_LUN_bySCSIid(self.SCSIid):
            util.SMlog("Unable to detect LUN")
            raise xs_errors.XenError('InvalidDev')
        self._pathrefresh(OCFSoISCSISR)
        out = OCFSSR.OCFSSR.probe(self)
        self.iscsi.detach(self.uuid)
        return out

    def vdi(self, uuid, loadLocked=False):
        return OCFSoISCSIVDI(self, uuid)
    
class OCFSoISCSIVDI(OCFSSR.OCFSFileVDI):
    def generate_config(self, sr_uuid, vdi_uuid):
        util.SMlog("OCFSoISCSIVDI.generate_config")
        dict = {}
        self.sr.dconf['localIQN'] = self.sr.iscsi.localIQN
        self.sr.dconf['multipathing'] = self.sr.mpath
        self.sr.dconf['multipathhandle'] = self.sr.mpathhandle
        dict['device_config'] = self.sr.dconf
        if dict['device_config'].has_key('chappassword_secret'):
            s = util.get_secret(self.session, dict['device_config']['chappassword_secret'])
            del dict['device_config']['chappassword_secret']
            dict['device_config']['chappassword'] = s
        dict['sr_uuid'] = sr_uuid
        dict['vdi_uuid'] = vdi_uuid
        dict['command'] = 'vdi_attach_from_config'
        # Return the 'config' encoded within a normal XMLRPC response so that
        # we can use the regular response/error parsing code.
        config = xmlrpclib.dumps(tuple([dict]), "vdi_attach_from_config")
        return xmlrpclib.dumps((config,), "", True)

    def attach_from_config(self, sr_uuid, vdi_uuid):
        util.SMlog("OCFSoISCSIVDI.attach_from_config")
        try:
            self.sr.iscsi.attach(sr_uuid)
            if not self.sr.iscsi._attach_LUN_bySCSIid(self.sr.SCSIid):
                raise xs_errors.XenError('InvalidDev')
            return OCFSSR.OCFSFileVDI.attach(self, sr_uuid, vdi_uuid)
        except:
            util.logException("OCFSoISCSIVDI.attach_from_config")
            raise xs_errors.XenError('SRUnavailable', \
                        opterr='Unable to attach the heartbeat disk')


if __name__ == '__main__':
    SRCommand.run(OCFSoISCSISR, DRIVER_INFO)
else:
    SR.registerSR(OCFSoISCSISR)
