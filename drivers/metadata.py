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
# Metadata VDI format
#

from xml.dom import minidom, Node
import struct
import sys, string
import util

HDR_STRING = "XSSM"
STRUCT_FMT = "%dsIHH" % len(HDR_STRING)
STRUCT_SIZE = struct.calcsize(STRUCT_FMT)
MD_MAJOR = 1
MD_MINOR = 2
XML_TAG = "SRMetadata"

def buildHeader(len):
    output = struct.pack(STRUCT_FMT, \
                         HDR_STRING, \
                         len, \
                         MD_MAJOR, \
                         MD_MINOR)
    return output

def unpackHeader(input):
    return struct.unpack(STRUCT_FMT,input)

def _testHdr(hdr):
    assert(hdr[0] == HDR_STRING)
    assert(hdr[2] <= MD_MAJOR)
    assert(hdr[3] <= MD_MINOR)

def unpackBody(input, len):
    return struct.unpack("%ds" % len,input)

def _generateXMLloop(Dict, e, dom):
    for key in Dict.keys():
        entry = dom.createElement(key)
        e.appendChild(entry)
        if not isinstance(Dict[key], dict):
            textnode = dom.createTextNode(str(Dict[key]))
            entry.appendChild(textnode)
        else:
            _generateXMLloop(Dict[key], entry, dom)
    
def _generateXML(Dict):
    dom = minidom.Document()
    md = dom.createElement(XML_TAG)
    dom.appendChild(md)

    _generateXMLloop(Dict, md, dom)
    return dom.toprettyxml()

def _walkXML(parent):
    Dict = {}
    
    if not parent.hasChildNodes():
        if parent.nodeValue == None:
            return ''

    for node in parent.childNodes:
        if node.nodeType == Node.ELEMENT_NODE:
            # Print the element name
            Dict[util.to_plain_string(node.nodeName)] = ""
            
            # Walk over any text nodes in the current node
            content = []
            for child in node.childNodes:
                if child.nodeType == Node.TEXT_NODE and \
                       child.nodeValue.strip():
                    content.append(child.nodeValue.strip())
            if content:
                strContent = string.join(content)
                Dict[util.to_plain_string(node.nodeName)] = util.to_plain_string(strContent)
            else:
                # Walk the child nodes
                Dict[util.to_plain_string(node.nodeName)] = _walkXML(node)
                
    return Dict

def _parseXML(str):
    dom = minidom.parseString(str)
    objectlist = dom.getElementsByTagName(XML_TAG)[0]
    Dict = _walkXML(objectlist)
    return Dict

def buildOutput(Dict):
    xmlstr = _generateXML(Dict)
    XML = struct.pack("%ds" % len(xmlstr),xmlstr)
    HDR = buildHeader(len(XML) + STRUCT_SIZE)
    return HDR + XML

def requiresUpgrade(path):
    f = open(path, "rb")
    s = f.read(STRUCT_SIZE)
    assert(len(s) == STRUCT_SIZE)
    hdr = unpackHeader(s)
    mdmajor = hdr[2]
    mdminor = hdr[3]
        
    if mdmajor < MD_MAJOR:
        return True
        
    if mdmajor == MD_MAJOR and mdminor < MD_MINOR:
        return True
        
    return False
    
def retrieveXMLfromFile(path):
    try:
        f = open(path, "rb")
        s = f.read(STRUCT_SIZE)
        assert(len(s) == STRUCT_SIZE)
        hdr = unpackHeader(s)
        _testHdr(hdr)
        xmllen = hdr[1] - STRUCT_SIZE
        s = f.read(xmllen)            
        assert(len(s) == xmllen)
        XML = unpackBody(s, xmllen)
        return XML[0]
    
    finally:
        f.close()        

def writeXMLtoFile(path, dict):
    f = open(path, "wb")
    f.write(buildOutput(dict))
    f.close()
    
def main():
    path = sys.argv[1]
    xml = retrieveXMLfromFile(path)
    print xml
    print _parseXML(xml)

if __name__ == '__main__':
    main()
