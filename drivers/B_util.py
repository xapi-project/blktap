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
# Miscellaneous utility functions for Borehamwood project
#

import os
import scsiutil

def refreshdev(pathlist):
    """
    Refresh block devices given a path list
    """
    # This function could fit into scsiutil
    for path in pathlist:
        dev = scsiutil.getdev(path)
        sysfs = os.path.join('/sys/block',dev,'device/rescan')
        if os.path.exists(sysfs):
            try:
                f = os.open(sysfs, os.O_WRONLY)
                os.write(f,'1')
                os.close(f)
            except:
                pass


def is_vdi_attached(session, vdi_ref):
    """ Check if a vdi is attached to a vm"""
    # This function could fit into util.py

    vbd_attached = False

    # vdi is considered attached if there is a VBD and if one the 
    # VBDs currently_attached flag is True
    vbds = session.xenapi.VBD.get_all_records_where( \
                                   "field \"VDI\" = \"%s\"" % vdi_ref)
    # Check if the vbd is in attached state
    for vbd_rec in vbds.values():
        if vbd_rec["currently_attached"]:
            vbd_attached = True
            break
    return vbd_attached

