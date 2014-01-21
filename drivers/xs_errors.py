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
# Xensource error codes
#

import os
import xml.dom.minidom
import SR
import util

XML_DEFS = '/opt/xensource/sm/XE_SR_ERRORCODES.xml'
class XenError(Exception):
    def __init__(self, key, opterr=None):
        # Check the XML definition file exists
        if not os.path.exists(XML_DEFS):
            print "No XML def file found"
            raise Exception.__init__(self, '')

        # Read the definition list
        self._fromxml('SM-errorcodes')

        ########DEBUG#######
        #for val in self.errorlist.keys():
        #    subdict = self.errorlist[val]
        #    print "KEY [%s]" % val
        #    for subval in subdict.keys():
        #        print "\tSUBKEY: %s, VALUE: %s" % (subval,subdict[subval])
        ########END#######

        # Now find the specific error
        if self.errorlist.has_key(key):
            subdict = self.errorlist[key]
            errorcode = int(subdict['value'])
            errormessage = subdict['description']
            if opterr is not None:
                errormessage += " [opterr=%s]" % opterr
            util.SMlog("Raising exception [%d, %s]" % (errorcode, errormessage))
            raise SR.SROSError(errorcode, errormessage)

        # development error
        raise SR.SROSError(1, "Error reporting error, unknown key %s" % key)
            

    def _fromxml(self, tag):
        dom = xml.dom.minidom.parse(XML_DEFS)
        objectlist = dom.getElementsByTagName(tag)[0]

        self.errorlist = {}
        for node in objectlist.childNodes:
            taglist = {}
            newval = False
            for n in node.childNodes:
                if n.nodeType == n.ELEMENT_NODE and node.nodeName == 'code':
                    taglist[n.nodeName] = ""
                    for e in n.childNodes:
                        if e.nodeType == e.TEXT_NODE:
                            newval = True
                            taglist[n.nodeName] += e.data
            if newval:
                name = taglist['name']
                self.errorlist[name] = taglist

