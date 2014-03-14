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

import xen.lowlevel.xs

def get_xs_handle():
    return xen.lowlevel.xs.xs()

def dirlist(h, base):
    return h.ls('', base)

def getval(h, path):
    return h.read('', path)

def setval(h, path, value):
    try:
        if h.write('', path, value) == None:
            return True
        else:
            return False
    except:
        return False

def xs_exists(h, path):
    try:
        if getval(h, path) != None:
            return True
        else:
            return False
    except:
        return False

def remove_xs_entry(h, dom_uuid, dom_path):
    path = "/vss/%s/%s" %(dom_uuid, dom_path)
    if xs_exists(h, path):
        try:
            h.rm('', path)
        except:
            raise "Unable to remove xenstore-node"
    else:
        raise "Invalid dom and path specified"

def set_watch(h, path):
    return h.watch(path, '')

def unwatch(h, path):
    return h.unwatch(path, '')
