#!/usr/bin/env python
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
# MPP LUNs path status update daemon

import sys
import os
import util
import glob
import time
import XenAPI
import mpath_dmp
import mpp_mpathutil
import gc

DEBUG_OUT = False
DAEMONISE = True
VERSION = "v1.0.1"
MPP_PATH_STATUS_UPDATE_INTERVAL = 60 
MPP_DEVICES_IN_USE_LOCATION = "/dev/disk/mpInuse"
MPP_PATH_KEY_PREFIX = "mpath-"
MPP_PATH_CACHE_LOCATION = "/tmp"

def help():
    print "updatempppathd version %s:\n" % VERSION
    print "Usage: updatempppathd -f : run in foreground"
    print "                      -d : send debug output to SMlog"
    sys.exit(-1)
    
def DEBUG(str):
    if DEBUG_OUT:
        util.SMlog(str, ident="updatempppathd", priority=util.LOG_DEBUG)

# Main update paths routine
def UpdatePaths():

    while(True):
	try:
    	    session = None
	    # Garbage collect any memory allocated in the last run of the loop
	    DEBUG("The garbage collection routine returned: %d" % gc.collect())

       	    # Sleep for some time before checking the status again
      	    time.sleep(MPP_PATH_STATUS_UPDATE_INTERVAL);	

	    # List the contents of the directory /dev/disk/mpInuse
            fileList = glob.glob(MPP_DEVICES_IN_USE_LOCATION + "/" + "*");
            if not len(fileList):
                continue

	    # for each SCSI ID get the cached values of the total paths and active paths 
	    # and then compare this with the current path status obtained using mpp_mpathutil.py
	    for filename in fileList:
                # extract the SCSI ID from the file name. 
                scsiid = filename.rsplit("/")[len(filename.rsplit("/")) - 1].split('-')[0]
                links=glob.glob('/dev/disk/by-mpp/%s' % scsiid)
                if not (len(links)):
                    continue
	        
		# Get the cached value for the total and active paths for this SCSI ID
		try:
		    cacheFile = glob.glob(MPP_PATH_CACHE_LOCATION + "/" + scsiid + "*");
		    if len(cacheFile) > 1:
			DEBUG("More than one cache file found for SCSI ID %s. Please check the cache manually.")
			raise Exception
		    
		    # This will return only one file name of the form SCSIID:TOTALPATHS:ACTIVEPATHS, so parse accordingly
		    cachedTotalPaths = cacheFile[0].split(":")[1];
		    cachedActivePaths = cacheFile[0].split(":")[2];
		    cacheFileFound = True
		except:
		    DEBUG("There was an exception getting the cached path status for SCSI ID %s, assuming 0s." % scsiid)
		    cachedTotalPaths = 0
		    cachedActivePaths = 0	
		    cacheFileFound = False

		(totalPaths, activePaths) = mpp_mpathutil.get_pathinfo(scsiid)

		DEBUG("For SCSI ID %s, cached TotalPaths: %s, cached ActivePaths: %s, New Totalpaths: %s New ActivePaths: %s" % (scsiid, cachedTotalPaths, cachedActivePaths, totalPaths, activePaths))

		if cachedTotalPaths != str(totalPaths) or cachedActivePaths != str(activePaths):
		    DEBUG("Some path status has changed for SCSI ID %s, updating PBD." % scsiid) 
		    entry = "[" + str(activePaths) + ", " + str(totalPaths) + "]"
                    DEBUG(entry)
                    cmd = ['/opt/xensource/sm/mpathcount.py', scsiid, entry]
                    util.pread2(cmd)

		    # Now update the cache with this updated path status
		    DEBUG("Checking if cache file was found.")
		    if cacheFileFound == True:
		        DEBUG("Cache file was found, delete it.")
		        os.remove(cacheFile[0])
		    cacheFileName = MPP_PATH_CACHE_LOCATION + "/" + scsiid + ":" + str(totalPaths) + ":" + str(activePaths)
		    DEBUG("Generated new cache file name %s" % cacheFileName)
		    open(cacheFileName, 'w').close()
			
	except:
            pass
  	    #DEBUG("There was some exception while updating path status for the SCSI ID %s." % scsiid )
	    #break
	
	if session != None:
            session.xenapi.session.logout()
		
    return

# Test Cmdline args
if len(sys.argv) > 1:
    for i in range(1,len(sys.argv)):        
        if sys.argv[i] == "-f":
            DAEMONISE = False
        elif sys.argv[i] == "-d":
            DEBUG_OUT = True
            try:
                DEBUG("UPDATEMPPPATHD - Daemon started (%s)" % VERSION)
            except:
                print "Logging failed"
                help()       

# Daemonize
if DAEMONISE:
    util.daemon()

try:
    UpdatePaths();
    
except:
    pass

DEBUG("UPDATEMPPPATHD - Daemon halted")
