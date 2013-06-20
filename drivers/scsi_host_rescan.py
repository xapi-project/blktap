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
# This module implements a safe way to rescan a scsi host so that:
# - at any time there is at most 1 rescan happening on a system for a hostid
# - we have as few rescans as possible 

import lock
import util
import os
import time
import glob
from datetime import datetime
from xmlrpclib import DateTime

HOST_LOCK_NAME_FORMAT = 'host%s'
RESCAN_LOCK_NAME = 'rescan'
START_TIME_FILE_PATH_FORMAT = '/var/run/host%s_starttime_%s'

def _rescan_hostID(host):
    util.SMlog("Performing rescan of host ID %s" % host)
    path = '/sys/class/scsi_host/host%s/scan' % host
    if os.path.exists(path):
        try:
            scanstring = "- - -"
            f=open(path, 'w')
            f.write('%s\n' % scanstring)
            f.close()
            # allow some time for undiscovered LUNs/channels to appear
            time.sleep(2)
        except Exception, e:
            util.SMlog("Failed to perform full rescan of host: %s. "\
                   "Error: %s" % (host, str(e)))
            raise Exception(str(e))
            
def rescan(hostid):
    try:
        try:
            # get the current time, call it x
            curr_time = datetime.utcnow()
            
            # acquire common lock
            l = lock.Lock(RESCAN_LOCK_NAME, HOST_LOCK_NAME_FORMAT % hostid)
            l.acquire()
            
            while(1):
                # check if starttime_anything exists
                tryRescan = False
                files = glob.glob(START_TIME_FILE_PATH_FORMAT % (hostid, '*'))
                if len(files) == 0:
                    # if not, create starttime_x
                    path = START_TIME_FILE_PATH_FORMAT % (hostid, str(curr_time))
                    path = path.replace(' ', '_')
                    open(path, 'w').close()
                    
                    # release common lock
                    l.release()
                    
                    # perform host rescan
                    _rescan_hostID(hostid)
                    
                    # acquire common lock
                    l.acquire()
                
                    # remove starttime_x
                    os.unlink(path)
                    
                    # release common lock and exit 
                    l.release()
                    break
                else:
                    # if it does
                    # read the start time 
                    start_time = files[0].split(START_TIME_FILE_PATH_FORMAT % (hostid, ''))[1]
                    start_time = DateTime(start_time.replace('__', ' '))                     
                    
                    while(1):
                        # stick around till start_time exists
                        # drop common lock
                        l.release()
                    
                        # sleep for a sec
                        time.sleep(1)
                
                        # acquire common lock
                        l.acquire()
                    
                        # check if start time exists
                        if len(glob.glob(START_TIME_FILE_PATH_FORMAT % \
                                         (hostid, '*'))) == 0:
                            tryRescan = False
                            if DateTime(str(curr_time)) < start_time:
                                # we are cool, this started before the rescan
                                # drop common lock and go home
                                l.release()                                
                            else:
                                # try to start a rescan
                                tryRescan = True
                            break
                        # else continue by default
                
                if not tryRescan:
                    break
                
        except Exception, e:
            util.SMlog("Failed to perform rescan of host: %s. "\
                       "Error: %s" % (hostid, str(e)))
    finally:
        l.release()
        