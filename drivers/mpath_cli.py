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
# Talk to the multipathd cli

import util
import re
import exceptions
import time

class MPathCLIFail(exceptions.Exception):
	def __init__(self):
		return
		
	def __str__(self):
		print "","MPath CLI failed"

mpathcmd = ["multipathd","-k"]

def mpexec(cmd):
    util.SMlog("mpath cmd: %s" % cmd)
    (rc,stdout,stderr) = util.doexec(mpathcmd,cmd)
    if stdout != "multipathd> ok\nmultipathd> " \
            and stdout != "multipathd> "+cmd+"\nok\nmultipathd> ":
        raise MPathCLIFail

def add_path(path):
    mpexec("add path %s" % path)

def remove_path(path):
    mpexec("remove path %s" % path)

def remove_map(m):
    mpexec("remove map %s" % m)

def resize_map(m):
    mpexec("resize map %s" % m)

# Don't reconfigure!!
#def reconfigure():
#    mpexec("reconfigure")

regex = re.compile("[0-9]+:[0-9]+:[0-9]+:[0-9]+\s*([a-z]*)")
regex2 = re.compile("multipathd>(\s*[^:]*:)?\s+(.*)")
regex3 = re.compile("switchgroup")

def is_working():
    cmd="help"
    util.SMlog("mpath cmd: %s" % cmd)
    try:
        (rc,stdout,stderr) = util.doexec(mpathcmd,cmd)
	util.SMlog("mpath output: %s" % stdout)
	m=regex3.search(stdout)
	if m:
	    return True
	else:
            return False
    except:
        return False
        
def do_get_topology(cmd):
    util.SMlog("mpath cmd: %s" % cmd)
    (rc,stdout,stderr) = util.doexec(mpathcmd,cmd)
    util.SMlog("mpath output: %s" % stdout)
    lines = stdout.split('\n')[:-1]
    if len(lines):
	    m=regex2.search(lines[0])
	    lines[0]=str(m.group(2))
    return lines

def get_topology(scsi_id):
    cmd="show map %s topology" % scsi_id
    return do_get_topology(cmd)

def get_all_topologies():
    cmd="show topology"
    return do_get_topology(cmd)

def list_paths(scsi_id):
    lines = get_topology(scsi_id)
    matches = []
    for line in lines:
        m=regex.search(line)
        if(m):
            matches.append(m.group(1))
    return matches

def list_maps():
    cmd="list maps"
    util.SMlog("mpath cmd: %s" % cmd)
    (rc,stdout,stderr) = util.doexec(mpathcmd,cmd)
    util.SMlog("mpath output: %s" % stdout)
    return map(lambda x: x.split(' ')[0], stdout.split('\n')[1:-1])

def ensure_map_gone(scsi_id):
    while True:
        paths=list_paths(scsi_id)
	util.SMlog("list_paths succeeded")
	if len(paths)==0:
	    return
	time.sleep(1)




