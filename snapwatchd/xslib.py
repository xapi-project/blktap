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
from snapdebug import DEBUG

def get_xs_handle():
    """ Obtain a handle to interact with xenstore via the xen-lowlevel library. """
    DEBUG("Get a new xenstore handle.", "snapwatchd/xslib")
    return xen.lowlevel.xs.xs()

def dirlist(h, base):
    """ Get a directory listing of the xenstore nodes as specified by the `base` argument.
        h       : transaction handle
        base    : path to list

        Returns : list of sub-directory nodes
                  None if key doesn't exist
        Raises xen.lowlevel.xs.Error on error.
        """
    DEBUG("Get directory listing for xenstore-path: %s" % base, "snapwatchd/xslib")
    return h.ls('', base)

def getval(h, path):
    """ Obtain data from a xenstore path.
        h       : transaction handle
        path    : xenstore-path to node

        Returns data read from xenstore-tree.
        Raises xen.lowlevel.xs.Error on error.
        """
    DEBUG("Read xenstore value for key: %s" % path, "snapwatchd/xslib")
    return h.read('', path)

def setval(h, path, value):
    """ Set data at a specific xenstore node.
        h       : transaction handle
        path    : xenstore-path to write into
        value   : value to be written into xenstore-node

        Returns:
        True on success
        False on failure/exception
        """
    try:
        DEBUG("Set value: %s at xenstore-path %s" % (value, path), "snapwatchd/xslib")
        if h.write('', path, value) == None:
            return True
        else:
            return False
    except:
        DEBUG("Unable to write value[%s] at xenstore-path [%s]" % (value, path), "snapwatchd/xslib")
        return False

def xs_exists(h, path):
    """ Check if a specific xenstore-node exists.
        h       : transaction handle
        path    : xenstore-path

        Returns:
        True if path exists
        False if path doesn't exist
        """
    try:
        DEBUG("Checking if xenstore-path: [%s] exists" % path, "snapwatchd/xslib")
        if getval(h, path) != None:
            return True
        else:
            return False
    except:
        return False

def remove_xs_entry(h, dom_uuid, dom_path):
    """ Remove node from vss-xenstore-tree
        h           : transaction handle
        dom_uuid    : UUID of domain
        dom_path    : domain path on xenstore

        XENSTORE PATH: /vss/dom_uuid/dom_path

        Returns None on success
        Raises exception on failure
        """
    path = "/vss/%s/%s" %(dom_uuid, dom_path)
    DEBUG("Removing xenstore-entry [%s] from xenstore-tree" % path, "snapwatchd/xslib")
    if xs_exists(h, path):
        try:
            h.rm('', path)
        except Exception, e:
            raise Exception("xenstore-node %s remove failed, err: %s" % (path, e))

def set_watch(h, path):
    """ Wrapper to set watch on xenstore-path.
        h       : transaction handle
        path    : xenstore-path to set watch on

        Returns None on success
        Raises xen.lowlevel.xs.Error on error
        """
    DEBUG("Setting watch on %s in xenstore" % path, "snapwatchd/xslib")
    return h.watch(path, '')

def unwatch(h, path):
    """ Wrapper to unwatch a xenstore-node
        h       : transaction handle
        path    : xenstore-path to unwatch

        Returns None on success
        Raises xen.lowlevel.xs.Error on error
        """
    DEBUG("Unwatch %s from xenstore" % path, "snapwatchd/xslib")
    return h.unwatch(path, '')
