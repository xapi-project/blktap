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
# A plugin for enabling trim on LVM based SRs to free up storage space
# in Storage arrays.

import xml
import sys
import os
import time
import util
import lock
import lvhdutil
import vhdutil
import lvutil
import xs_errors
import xmlrpclib

TRIM_LV_TAG = "_trim_lv"
TRIM_CAP = "SR_TRIM"
LOCK_RETRY_ATTEMPTS = 3
LOCK_RETRY_INTERVAL = 1
ERROR_CODE_KEY = "errcode"
ERROR_MSG_KEY = "errmsg"

TRIM_LAST_TRIGGERED_KEY = "trim_last_triggered"
MASTER_LVM_CONF = '/etc/lvm/master'

def _vg_by_sr_uuid(sr_uuid):
    return lvhdutil.VG_PREFIX + sr_uuid

def _lvpath_by_vg_lv_name(vg_name, lv_name):
    return os.path.join(lvhdutil.VG_LOCATION, vg_name, lv_name)

def to_xml(d):

    dom = xml.dom.minidom.Document()
    trim_response = dom.createElement("trim_response")
    dom.appendChild(trim_response)

    for key, value in sorted(d.items()):
        key_value_element = dom.createElement("key_value_pair")
        trim_response.appendChild(key_value_element)

        key_element = dom.createElement("key")
        key_text_node = dom.createTextNode(key)
        key_element.appendChild(key_text_node)
        key_value_element.appendChild(key_element)

        value_element = dom.createElement("value")
        value_text_mode = dom.createTextNode(value)
        value_element.appendChild(value_text_mode)
        key_value_element.appendChild(value_element)


    return dom.toxml()

# Note: This function is expected to be called from a context where
# the SR is locked by the thread calling the function; therefore removing
# any risk of a race condition updating the LAST_TRIGGERED value.
def _log_last_triggered(session, sr_uuid):
    try:
        sr_ref = session.xenapi.SR.get_by_uuid(sr_uuid)
        other_config = session.xenapi.SR.get_other_config(sr_ref)
        if other_config.has_key(TRIM_LAST_TRIGGERED_KEY):
            session.xenapi.SR.remove_from_other_config(sr_ref, TRIM_LAST_TRIGGERED_KEY)
        session.xenapi.SR.add_to_other_config(sr_ref, TRIM_LAST_TRIGGERED_KEY, str(time.time()))
    except:
        util.logException("Unable to set other-config:%s" % TRIM_LAST_TRIGGERED_KEY)

def do_trim(session, args):
    """Attempt to trim the given LVHDSR"""
    util.SMlog("do_trim: %s" % args)
    sr_uuid = args["sr_uuid"]
    os.environ['LVM_SYSTEM_DIR'] = MASTER_LVM_CONF

    if TRIM_CAP not in util.sr_get_capability(sr_uuid):
        util.SMlog("Trim command ignored on unsupported SR %s" % sr_uuid)
        err_msg = {ERROR_CODE_KEY: 'UnsupportedSRForTrim',
                   ERROR_MSG_KEY: 'Trim on [%s] not supported' % sr_uuid}
        return to_xml(err_msg)

    # Lock SR, get vg empty space details
    sr_lock = lock.Lock(vhdutil.LOCK_TYPE_SR, sr_uuid)
    got_lock = False
    for i in range(LOCK_RETRY_ATTEMPTS):
        got_lock = sr_lock.acquireNoblock()
        if got_lock:
            break
        time.sleep(LOCK_RETRY_INTERVAL)

    if got_lock:
        try:
            vg_name = _vg_by_sr_uuid(sr_uuid)
            lv_name = sr_uuid + TRIM_LV_TAG
            lv_path = _lvpath_by_vg_lv_name(vg_name, lv_name)

            # Clean trim LV in case the previous trim attemp failed
            if lvutil.exists(lv_path):
                lvutil.remove(lv_path)

            # Perform a lvcreate, blkdiscard and lvremove to trigger trim on the array
            lvutil.create(lv_name, 0, vg_name, size_in_percentage="100%F")
            cmd = ["/usr/sbin/blkdiscard", "-v", lv_path]
            stdout = util.pread2(cmd)
            util.SMlog("Stdout is %s" % stdout)
            lvutil.remove(lv_path)
            util.SMlog("Trim on SR: %s complete. " % sr_uuid)
            result = str(True)
        except util.CommandException, e:
            err_msg = {
                ERROR_CODE_KEY: 'TrimException',
                ERROR_MSG_KEY: e.reason
            }
            result = to_xml(err_msg)
        except:
            err_msg = {
                ERROR_CODE_KEY: 'UnknownTrimException',
                ERROR_MSG_KEY: 'Unknown Exception: trim failed on SR [%s]'
                % sr_uuid
            }
            result = to_xml(err_msg)

        _log_last_triggered(session, sr_uuid)

        sr_lock.release()
        return result
    else:
        util.SMlog("Could not complete Trim on %s, Lock unavailable !" \
                   % sr_uuid)
        err_msg = {ERROR_CODE_KEY: 'SRUnavailable',
                   ERROR_MSG_KEY: 'Unable to get SR lock [%s]' % sr_uuid}
        return to_xml(err_msg)
