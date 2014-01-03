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
# SRCommand: parse SR command-line objects
#

import XenAPI
import sys, errno, syslog
import xs_errors
import xmlrpclib
import SR, VDI, util
import blktap2
import resetvdis
import os
import copy

NEEDS_VDI_OBJECT = [
        "vdi_update", "vdi_create", "vdi_delete", "vdi_snapshot", "vdi_clone",
        "vdi_resize", "vdi_resize_online", "vdi_attach", "vdi_detach",
        "vdi_activate", "vdi_deactivate", "vdi_attach_from_config",
        "vdi_generate_config", "vdi_compose",
        "vdi_epoch_begin", "vdi_epoch_end" ]

# don't log the commands that spam the log file too much
NO_LOGGING = {
        "iso": ["sr_scan"],
        "nfs_iso": ["sr_scan"]
}

EXCEPTION_TYPE = {
        "sr_scan"           : "SRScan",
        "vdi_init"          : "VDILoad",
        "vdi_create"        : "VDICreate",
        "vdi_delete"        : "VDIDelete",
        "vdi_attach"        : "VDIUnavailable",
        "vdi_detach"        : "VDIUnavailable",
        "vdi_activate"      : "VDIUnavailable",
        "vdi_deactivate"    : "VDIUnavailable",
        "vdi_resize"        : "VDIResize",
        "vdi_resize_online" : "VDIResize",
        "vdi_snapshot"      : "VDISnapshot",
        "vdi_clone"         : "VDIClone",
}


class SRCommand:
    def __init__(self, driver_info):
        self.dconf = ''
        self.type = ''
        self.sr_uuid = ''
        self.cmdname = ''
        self.cmdtype = ''
        self.cmd = None
        self.args = None
        self.driver_info = driver_info

    def parse(self):
        if len(sys.argv) <> 2:
            util.SMlog("Failed to parse commandline; wrong number of arguments; argv = %s" % (repr(sys.argv)))
            raise xs_errors.XenError('BadRequest') 

        # Debug logging of the actual incoming command from the caller.
        # util.SMlog( "" )
        # util.SMlog( "SM.parse: DEBUG: args = %s,\n%s" % \
        #             ( sys.argv[0], \
        #               util.splitXmlText( util.hideMemberValuesInXmlParams( \
        #                                  sys.argv[1] ), showContd=True ) ), \
        #                                  priority=syslog.LOG_DEBUG )

        try:
            params, methodname = xmlrpclib.loads(sys.argv[1])
            self.cmd = methodname
            params = params[0] # expect a single struct
            self.params = params

            # params is a dictionary
            self.dconf = params['device_config']
            if params.has_key('sr_uuid'):
                self.sr_uuid = params['sr_uuid']
            if params.has_key('vdi_uuid'):
                self.vdi_uuid = params['vdi_uuid']
            elif self.cmd == "vdi_create":
                self.vdi_uuid = util.gen_uuid ()
                
        except Exception, e:
            util.SMlog("Failed to parse commandline; exception = %s argv = %s" % (str(e), repr(sys.argv)))
            raise xs_errors.XenError('BadRequest')

    def run_statics(self):
        if self.params['command'] == 'sr_get_driver_info':
            print util.sr_get_driver_info(self.driver_info)
            sys.exit(0)

    def run(self, sr):
        try:
            return self._run_locked(sr)
        except (util.CommandException, util.SMException, XenAPI.Failure), e:
            util.logException(self.cmd)
            msg = str(e)
            if isinstance(e, util.CommandException):
                msg = "Command %s failed (%s): %s" % \
                        (e.cmd, e.reason, os.strerror(abs(e.code)))
            excType = EXCEPTION_TYPE.get(self.cmd)
            if not excType:
                excType = "SMGeneral"
            raise xs_errors.XenError(excType, opterr=msg)

        except blktap2.TapdiskFailed, e:
            util.logException('tapdisk failed exception: %s' % e)
            raise xs_errors.XenError('TapdiskFailed',
                os.strerror(e.get_error().get_error_code()))

        except blktap2.TapdiskExists, e:            
            util.logException('tapdisk exists exception: %s' % e)
            raise xs_errors.XenError('TapdiskAlreadyRunning', e.__str__())

        except:
            util.logException('generic exception: %s' % self.cmd)
            raise

    def _run_locked(self, sr):
        lockSR = False
        lockInitOnly = False
        rv = None
        e = None
        if self.cmd in sr.ops_exclusive:
            lockSR = True
        elif self.cmd in NEEDS_VDI_OBJECT and "vdi_init" in sr.ops_exclusive:
            lockInitOnly = True

        target = None
        acquired = False
        if lockSR or lockInitOnly:
            sr.lock.acquire()
            acquired = True
        try:
            try:
                if self.cmd in NEEDS_VDI_OBJECT:
                    target = sr.vdi(self.vdi_uuid)
            finally:
                if acquired and lockInitOnly:
                    sr.lock.release()
                    acquired = False
            try:
                rv = self._run(sr, target)
            except Exception, e:
                raise
        finally:
            if acquired:
                sr.lock.release()
            try:
                sr.cleanup()
            except Exception, e1:
                msg = 'failed to clean up SR: %s' % e1
                if not e:
                    util.SMlog(msg)
                    raise e1
                else:
                    util.SMlog('WARNING: %s (error ignored)' % msg)
        return rv

    def _run(self, sr, target):
        dconf_type = sr.dconf.get("type")
        if not dconf_type or not NO_LOGGING.get(dconf_type) or \
                not self.cmd in NO_LOGGING[dconf_type]:
            if 'device_config' in self.params:
                util.SMlog("%s %s" % (self.cmd, util.hidePasswdInParams(self.params,'device_config')))
            else:
                util.SMlog("%s %s" % (self.cmd, repr(self.params)))

        caching_params = dict((k, self.params.get(k)) for k in \
                [blktap2.VDI.CONF_KEY_ALLOW_CACHING,
                 blktap2.VDI.CONF_KEY_MODE_ON_BOOT,
                 blktap2.VDI.CONF_KEY_CACHE_SR])

        if self.cmd == 'vdi_create':
            # These are the fields owned by the backend, passed on the
            # commandline:

            # LVM SRs store their metadata in XML format. XML does not support
            # all unicode characters, so we must check if the label or the
            # description contain such characters. We must enforce this
            # restriction to other SRs as well (even if they do allow these
            # characters) in order to be consistent.
            target.label = self.params['args'][1]
            target.description = self.params['args'][2]

            if not util.isLegalXMLString(target.label) \
                    or not util.isLegalXMLString(target.description):
                raise xs_errors.XenError('IllegalXMLChar', \
                        opterr = 'The name and/or description you supplied contains one or more unsupported characters. The name and/or description must contain valid XML characters. See http://www.w3.org/TR/2004/REC-xml-20040204/#charsets for more information.')

            target.ty = self.params['vdi_type']
            target.metadata_of_pool = self.params['args'][3]
            target.is_a_snapshot = self.params['args'][4] == "true"
            target.snapshot_time = self.params['args'][5]
            target.snapshot_of = self.params['args'][6]
            target.read_only = self.params['args'][7] == "true"

            return target.create(self.params['sr_uuid'], self.vdi_uuid, long(self.params['args'][0]))

        elif self.cmd == 'vdi_update':
            # Check for invalid XML characters, similar to VDI.create right
            # above.

            vdi_ref = sr.session.xenapi.VDI.get_by_uuid(self.vdi_uuid)
            name_label = sr.session.xenapi.VDI.get_name_label(vdi_ref)
            description = sr.session.xenapi.VDI.get_name_description(vdi_ref)

            if not util.isLegalXMLString(name_label) \
                    or not util.isLegalXMLString(description):
                raise xs_errors.XenError('IllegalXMLChar', \
                        opterr = 'The name and/or description you supplied contains one or more unsupported characters. The name and/or description must contain valid XML characters. See http://www.w3.org/TR/2004/REC-xml-20040204/#charsets for more information.')

            return target.update(self.params['sr_uuid'], self.vdi_uuid)

        elif self.cmd == 'vdi_introduce':
            target = sr.vdi(self.params['new_uuid'])
            return target.introduce(self.params['sr_uuid'], self.params['new_uuid'])
        
        elif self.cmd == 'vdi_delete':
            return target.delete(self.params['sr_uuid'], self.vdi_uuid)

        elif self.cmd == 'vdi_attach':
            target = blktap2.VDI(self.vdi_uuid, target, self.driver_info)
            writable = self.params['args'][0] == 'true'
            return target.attach(self.params['sr_uuid'], self.vdi_uuid, writable)

        elif self.cmd == 'vdi_detach':
            target = blktap2.VDI(self.vdi_uuid, target, self.driver_info)
            return target.detach(self.params['sr_uuid'], self.vdi_uuid)

        elif self.cmd == 'vdi_snapshot':
            return target.snapshot(self.params['sr_uuid'], self.vdi_uuid)

        elif self.cmd == 'vdi_clone':
            return target.clone(self.params['sr_uuid'], self.vdi_uuid)            

        elif self.cmd == 'vdi_resize':
            return target.resize(self.params['sr_uuid'], self.vdi_uuid, long(self.params['args'][0]))

        elif self.cmd == 'vdi_resize_online':
            return target.resize_online(self.params['sr_uuid'], self.vdi_uuid, long(self.params['args'][0]))
        
        elif self.cmd == 'vdi_activate':
            target = blktap2.VDI(self.vdi_uuid, target, self.driver_info)
            writable = self.params['args'][0] == 'true'
            return target.activate(self.params['sr_uuid'], self.vdi_uuid,
                                   writable, caching_params)

        elif self.cmd == 'vdi_deactivate':
            target = blktap2.VDI(self.vdi_uuid, target, self.driver_info)
            return target.deactivate(self.params['sr_uuid'], self.vdi_uuid,
                    caching_params)

        elif self.cmd == 'vdi_epoch_begin':
            if caching_params.get(blktap2.VDI.CONF_KEY_MODE_ON_BOOT) != "reset":
                return
            if not "VDI_RESET_ON_BOOT/2" in self.driver_info['capabilities']:
                raise xs_errors.XenError('Unimplemented')
            return target.reset_leaf(self.params['sr_uuid'], self.vdi_uuid)

        elif self.cmd == 'vdi_epoch_end':
            return

        elif self.cmd == 'vdi_generate_config':
            return target.generate_config(self.params['sr_uuid'], self.vdi_uuid)

        elif self.cmd == 'vdi_compose':
            vdi1_uuid = sr.session.xenapi.VDI.get_uuid(self.params['args'][0])
            return target.compose(self.params['sr_uuid'], vdi1_uuid, self.vdi_uuid)

        elif self.cmd == 'vdi_attach_from_config':
            ret = target.attach_from_config(self.params['sr_uuid'], self.vdi_uuid)
            if not target.sr.driver_config.get("ATTACH_FROM_CONFIG_WITH_TAPDISK"):
                return ret
            target = blktap2.VDI(self.vdi_uuid, target, self.driver_info)
            return target.attach(self.params['sr_uuid'], self.vdi_uuid, True, True)

        elif self.cmd == 'sr_create':
            return sr.create(self.params['sr_uuid'], long(self.params['args'][0]))

        elif self.cmd == 'sr_delete':
            return sr.delete(self.params['sr_uuid'])

        elif self.cmd == 'sr_update':
            return sr.update(self.params['sr_uuid'])

        elif self.cmd == 'sr_probe':
            txt = sr.probe()
            util.SMlog( "sr_probe result: %s" % util.splitXmlText( txt, showContd=True ) )
            # return the XML document as a string
            return xmlrpclib.dumps((txt,), "", True)

        elif self.cmd == 'sr_attach':
            is_master = False
            if sr.dconf.get("SRmaster") == "true":
                is_master = True

            resetvdis.reset_sr(sr.session, util.get_this_host(),
                    self.params['sr_uuid'], is_master)

            if is_master:
                # Schedule a scan only when attaching on the SRmaster
                util.set_dirty(sr.session, self.params["sr_ref"])

            return sr.attach(self.params['sr_uuid'])

        elif self.cmd == 'sr_detach':
            return sr.detach(self.params['sr_uuid'])

        elif self.cmd == 'sr_content_type':
            return sr.content_type(self.params['sr_uuid'])        

        elif self.cmd == 'sr_scan':
            return sr.scan(self.params['sr_uuid'])

        else:
            util.SMlog("Unknown command: %s" % self.cmd)
            raise xs_errors.XenError('BadRequest')             

def run(driver, driver_info):
    """Convenience method to run command on the given driver"""
    cmd = SRCommand(driver_info)
    try:
        cmd.parse()
        cmd.run_statics()
        sr = driver(cmd, cmd.sr_uuid)
        sr.direct = True
        ret = cmd.run(sr)

        if ret == None:
            print util.return_nil ()
        else:
            print ret
        sys.exit(0)
        
    except SR.SRException, inst:
        print inst.toxml()
        sys.exit(0)
