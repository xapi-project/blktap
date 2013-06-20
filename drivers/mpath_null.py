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

import xs_errors

DEVBYIDPATH = "/dev/disk/by-id"

def refresh(sid,npaths):
    return

def reset(sid,explicit_unmap=False,delete_nodes=False):
    return

def activate():
    return

def deactivate():
    return

def path(SCSIid):
    return DEVBYIDPATH + "/scsi-" + SCSIid

def status(SCSIid):
    pass
