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
# Utility functions to query and list local physical block devices from /sys

import os, os.path, sys, time, stat

def read_whole_file(filename):
    f = open(filename, 'r')
    try:
        return f.readlines()
    finally:
        f.close()

def list():
    """List physical block devices from /sys"""
    all = os.listdir("/sys/block")
    def is_physical_device(dev):
         sys = os.path.join("/sys/block", dev)
         device = os.path.join(sys, "device")
	 return os.path.exists(device)
    return filter(is_physical_device, all)


def stat(device):
    """Given a device name, return a dictionary containing keys:
       size: size of device in bytes
       bus:  bus type (eg USB, IDE)
       bus_path: identifier of bus endpoint (eg 1:0:0)
       hwinfo: string containing vendor, model, rev and type information"""
    results = {}
    sys = os.path.join("/sys/block", device)
    device = os.path.join(sys, "device")
    
    try:
        results["size"] = long(read_whole_file(os.path.join(sys, "size"))[0]) * 512L
    except:
        pass

    results["bus"] = "Unrecognised bus type"
    results["bus_path"] = ""
    try:
        device_path = os.readlink(device)
        if device_path.find("/usb") <> -1:
            results["bus"] = "USB"
            results["bus_path"] = os.path.basename(device_path)
        elif device_path.find("/ide") <> -1:
            results["bus"] = "IDE"
            results["bus_path"] = os.path.basename(device_path)
        elif os.readlink(os.path.join(device, "bus")).endswith("scsi"):
            results["bus"] = "SCSI"
            all = os.listdir(device)
            # Read the identifier after "scsi_device:"
            disk = filter(lambda x:x.startswith("scsi_device:"), all)[0]
            results["bus_path"] = disk[len("scsi_device:"):]
    except:
        pass

    # Work out the vendor/model/rev info
    results["hwinfo"] = ""

    for field,fmt in [("vendor", "%s"), ("model", "model %s"), ("rev", "rev %s"), ("type", "type %s")]:
        try:
            value = read_whole_file(os.path.join(device, field))[0].strip()
            value = fmt % value
            if results["hwinfo"] != "":
                results["hwinfo"] = results["hwinfo"] + " " + value
            else:
                results["hwinfo"] = value
        except:
            pass

    return results

