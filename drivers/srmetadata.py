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
# Functions to read and write SR metadata 
#
import util
import metadata
import os
import sys
sys.path.insert(0,'/opt/xensource/sm/snapwatchd')
from xslib import get_min_blk_size, open_file_for_write, open_file_for_read, \
    xs_file_write, xs_file_read, close_file
import xs_errors
import lvutil
import xml.sax.saxutils

SECTOR_SIZE = 512
XML_HEADER = "<?xml version=\"1.0\" ?>"
SECTOR2_FMT= "%s%s%s" 
MAX_METADATA_LENGTH_SIZE = 10
LEN_FMT = "%" + "-%ds" % MAX_METADATA_LENGTH_SIZE
SECTOR_STRUCT = "%-512s" 
OFFSET_TAG = 'offset'

# define xml tags for metadata 
ALLOCATION_TAG = 'allocation'
NAME_LABEL_TAG = 'name_label'
NAME_DESCRIPTION_TAG = 'name_description'
VDI_TAG = 'vdi'
VDI_CLOSING_TAG = '</%s>' % VDI_TAG
VDI_DELETED_TAG = 'deleted'
UUID_TAG = 'uuid'
IS_A_SNAPSHOT_TAG = 'is_a_snapshot'
SNAPSHOT_OF_TAG = 'snapshot_of'
TYPE_TAG = 'type'
VDI_TYPE_TAG = 'vdi_type'
READ_ONLY_TAG = 'read_only'
MANAGED_TAG = 'managed'
SNAPSHOT_TIME_TAG = 'snapshot_time'
METADATA_OF_POOL_TAG = 'metadata_of_pool'
SVID_TAG = 'svid'
LUN_LABEL_TAG = 'll'
VDI_SECTOR_1 = "<%s><%s>%s</%s><%s>%s</%s>" % (VDI_TAG,
                                               NAME_LABEL_TAG,
                                               '%s',
                                               NAME_LABEL_TAG,
                                               NAME_DESCRIPTION_TAG,
                                               '%s',
                                               NAME_DESCRIPTION_TAG)
MAX_VDI_NAME_LABEL_DESC_LENGTH = SECTOR_SIZE - 2*len(NAME_LABEL_TAG) - \
    2*len(NAME_DESCRIPTION_TAG) - len(VDI_TAG) - 12

ATOMIC_UPDATE_PARAMS_AND_OFFSET = {NAME_LABEL_TAG: 2,
                                        NAME_DESCRIPTION_TAG: 3}
SR_INFO_SIZE_IN_SECTORS = 4
HEADER_SEP = ':'
METADATA_UPDATE_OBJECT_TYPE_TAG = 'objtype'
METADATA_OBJECT_TYPE_SR = 'sr'
METADATA_OBJECT_TYPE_VDI = 'vdi'

# ----------------- # General helper functions - begin # -----------------
def get_min_blk_size_wrapper(fd):
    result = get_min_blk_size(fd)
    if result.result == -1:
        raise "Failed to get minimum block size for the metadata file. "\
            "Error: %s" % os.strerror(result.err) 
    else:
        return result.result
    
def open_file(path, write = False): 
    if write:
        result = open_file_for_write(path)
        if result.result == -1:
            raise IOError("Failed to open file %s for write. Error: %s" % \
                          (path, os.strerror(result.err)))
    else:
        result = open_file_for_read(path)
        if result.result == -1:
            raise IOError("Failed to open file %s for read. Error: %s" % \
                          (path, os.strerror(result.err)))

    return result.result

def xs_file_write_wrapper(fd, offset, blocksize, data, length):
    result = xs_file_write(fd, offset, blocksize, data, length)
    if result.result == -1:
        raise IOError("Failed to write file with params %s. Error: %s" % \
                          ([fd, offset, blocksize, data, length], \
                            os.strerror(result.err)))
    return result.result

def xs_file_read_wrapper(fd, offset, bytesToRead, min_block_size):
    result = xs_file_read(fd, offset, bytesToRead, min_block_size)
    if result.result == -1:
        util.SMlog("Failed to read file with params %s. Error: %s" % \
                          ([fd, offset, bytesToRead, min_block_size], \
                            os.strerror(result.err)))
        util.SMlog("Return from read: result: %d, readString: %s, "
                   "noOfBytesRead: %d, err: %s" % (result.result,
                   result.readString,
                   result.noOfBytesRead,
                   os.strerror(result.err)))
        
        raise IOError("Failed to read file with params %s. Error: %s" % \
                          ([fd, offset, bytesToRead, min_block_size], \
                            os.strerror(result.err)))
    return result.readString
        
def close(fd):
    if fd != -1:
        close_file(fd)

# get a range which is block aligned, contains 'offset' and allows
# length bytes to be written
def getBlockAlignedRange(block_size, offset, length):
    lower = 0
    if offset%block_size == 0:
        lower = offset
    else:
        lower = offset - offset%block_size
        
    upper = lower + block_size
    
    while upper < (lower + length):
        upper += block_size
        
    return (lower, upper)

def buildHeader(len, major = metadata.MD_MAJOR, minor = metadata.MD_MINOR):
    # build the header, which is the first sector
    output = ("%s%s%s%s%s%s%s" % (metadata.HDR_STRING,
                                   HEADER_SEP,
                                   LEN_FMT,
                                   HEADER_SEP,
                                   str(major),
                                   HEADER_SEP,
                                   str(minor)
                                   )) % len
    return output

def unpackHeader(input):
    vals = input.split(HEADER_SEP)
    return (vals[0], vals[1], vals[2], vals[3])

def getSector(str):
    sector = SECTOR_STRUCT % str
    return sector
    
def getSectorAlignedXML(tagName, value):
    # truncate data if we breach the 512 limit
    if len("<%s>%s</%s>" % (tagName, value, tagName)) > SECTOR_SIZE:
        length = util.unictrunc(value, SECTOR_SIZE - 2*len(tagName) - 5)
        util.SMlog('warning: SR ' + tagName + ' truncated from ' \
                + str(len(value)) + ' to ' + str(length) + ' bytes')
        value = value[:length]
        
    return "<%s>%s</%s>" % (tagName, value, tagName)
    
def getXMLTag(tagName):
        return "<%s>%s</%s>" % (tagName, '%s', tagName)
    
def updateLengthInHeader(fd, length, major = metadata.MD_MAJOR, \
                         minor = metadata.MD_MINOR):
    try:
        min_block_size = get_min_blk_size_wrapper(fd)
        md = ''
        md = xs_file_read_wrapper(fd, 0, min_block_size, min_block_size)
        updated_md = buildHeader(length, major, minor)
        updated_md += md[SECTOR_SIZE:]
        
        # Now write the new length
        xs_file_write_wrapper(fd, 0, min_block_size, updated_md, len(updated_md))
    except Exception, e:
        util.SMlog("Exception updating metadata length with length: %d." 
                   "Error: %s" % (length, str(e)))
        raise
   
def getMetadataLength(fd):
    try:
        min_blk_size = get_min_blk_size_wrapper(fd)
        sector1 = xs_file_read_wrapper(fd, 0, SECTOR_SIZE, min_blk_size)
        lenstr = sector1.split(HEADER_SEP)[1]
        len = int(lenstr.strip(' '))
        return len
    except Exception, e:
        util.SMlog("Exception getting metadata length." 
                   "Error: %s" % str(e))
        raise
    
def requiresUpgrade(path):
    # if the metadata requires upgrade either by pre-Boston logic
    # or Boston logic upgrade it
    
    # First check if this is a pre-6.0 pool using the old metadata
    pre_boston_upgrade = False
    boston_upgrade = False
    try:
        if metadata.requiresUpgrade(path):
            pre_boston_upgrade = True
    except Exception, e:
        util.SMlog("This looks like a 6.0 or later pool, try checking " \
                   "for upgrade using the new metadata header format. " \
                    "Error: %s" % str(e))
      
    try:  
        # Now check for upgrade using the header format for 6.0/post-6.0
        try:
            fd = -1
            fd = open_file(path)
            min_blk_size = get_min_blk_size_wrapper(fd)
            sector1 = \
                xs_file_read_wrapper(fd, 0, SECTOR_SIZE, min_blk_size).strip()
            hdr = unpackHeader(sector1)
            mdmajor = int(hdr[2])
            mdminor = int(hdr[3])
               
            if mdmajor < metadata.MD_MAJOR:
                boston_upgrade = True
            elif mdmajor == metadata.MD_MAJOR and mdminor < metadata.MD_MINOR:
                boston_upgrade = True
           
        except Exception, e:
            util.SMlog("Exception checking header version, upgrading metadata."\
                       " Error: %s" % str(e))
            return True
    finally:
        close(fd)
        
    return pre_boston_upgrade or boston_upgrade

# ----------------- # General helper functions - end # -----------------
class MetadataHandler:

    VDI_INFO_SIZE_IN_SECTORS = None

    # constructor
    def __init__(self, path = None, write = True):

        self.fd = -1
        self.path = path
        if self.path != None:
            self.fd = open_file(self.path, write)
    
    def __del__(self):
        if self.fd != -1:
            close(self.fd)

    def spaceAvailableForVdis(self, count):
        raise NotImplementedError("spaceAvailableForVdis is undefined")
            
    # common utility functions
    def getMetadata(self,params = {}):
        try:
            sr_info = {}
            vdi_info = {}
            try:
                md = self.getMetadataInternal(params)
                sr_info = md['sr_info']
                vdi_info = md['vdi_info']
            except:
                # Maybe there is no metadata yet
                pass
            
        except Exception, e:
            util.SMlog('Exception getting metadata. Error: %s' % str(e))
            raise xs_errors.XenError('MetadataError', \
                         opterr='%s' % str(e))
        
        return (sr_info, vdi_info)
    
    def writeMetadata(self, sr_info, vdi_info):
        try:
            self.writeMetadataInternal(sr_info, vdi_info)
        except Exception, e:
            util.SMlog('Exception writing metadata. Error: %s' % str(e))
            raise xs_errors.XenError('MetadataError', \
                         opterr='%s' % str(e))
        
    # read metadata for this SR and find if a metadata VDI exists 
    def findMetadataVDI(self):
        try:
            vdi_info = self.getMetadata()[1]
            for offset in vdi_info.keys():
                if vdi_info[offset][TYPE_TAG] == 'metadata' and \
                    vdi_info[offset][IS_A_SNAPSHOT_TAG] == '0':
                        return vdi_info[offset][UUID_TAG]
            
            return None
        except Exception, e:
            util.SMlog('Exception checking if SR metadata a metadata VDI.'\
                       'Error: %s' % str(e))
            raise xs_errors.XenError('MetadataError', \
                         opterr='%s' % str(e))
            
    # update the SR information or one of the VDIs information
    # the passed in map would have a key 'objtype', either sr or vdi.
    # if the key is sr, the following might be passed in
    #   SR name-label
    #   SR name_description
    # if the key is vdi, the following information per VDI may be passed in
    #   uuid - mandatory
    #   name-label
    #   name_description
    #   is_a_snapshot
    #   snapshot_of, if snapshot status is true
    #   snapshot time
    #   type: system, user or metadata etc
    #   vdi_type: raw or vhd
    #   read_only
    #   location
    #   managed
    #   metadata_of_pool
    def updateMetadata(self, update_map = {}):
        util.SMlog("Updating metadata : %s" % update_map)

        try:
            objtype = update_map[METADATA_UPDATE_OBJECT_TYPE_TAG]
            del update_map[METADATA_UPDATE_OBJECT_TYPE_TAG]
            
            if objtype == METADATA_OBJECT_TYPE_SR:
                self.updateSR(update_map)
            elif objtype == METADATA_OBJECT_TYPE_VDI: 
                self.updateVdi(update_map)
        except Exception, e:
            util.SMlog('Error updating Metadata Volume with update' \
                         'map: %s. Error: %s' % (update_map, str(e)))
            raise xs_errors.XenError('MetadataError', \
                         opterr='%s' % str(e))
            
    def deleteVdiFromMetadata(self, vdi_uuid):
        util.SMlog("Deleting vdi: %s" % vdi_uuid)
        try:
            self.deleteVdi(vdi_uuid)
        except Exception, e:
            util.SMlog('Error deleting vdi %s from the metadata. '\
                'Error: %s' % (vdi_uuid, str(e)))
            raise xs_errors.XenError('MetadataError', \
                opterr='%s' % str(e))
        
    def addVdi(self, vdi_info = {}):
        util.SMlog("Adding VDI with info: %s" % vdi_info)
        try:
            self.addVdiInternal(vdi_info)
        except Exception, e:
            util.SMlog('Error adding VDI to Metadata Volume with '\
                'update map: %s. Error: %s' % (vdi_info, str(e)))
            raise xs_errors.XenError('MetadataError', \
                opterr='%s' % (str(e)))
            
    def ensureSpaceIsAvailableForVdis(self, count):
        util.SMlog("Checking if there is space in the metadata for %d VDI." % \
                   count)
        try:
            self.spaceAvailableForVdis(count)
        except Exception, e:
            raise xs_errors.XenError('MetadataError', \
                opterr='%s' % str(e))
        
    # common functions
    def deleteVdi(self, vdi_uuid, offset = 0):
        util.SMlog("Entering deleteVdi")
        try:
            md = self.getMetadataInternal({'vdi_uuid': vdi_uuid})
            if not md.has_key('offset'):
                util.SMlog("Metadata for VDI %s not present, or already removed, " \
                    "no further deletion action required." % vdi_uuid)
                return
            
            md['vdi_info'][md['offset']][VDI_DELETED_TAG] = '1'
            self.updateVdi(md['vdi_info'][md['offset']])
            
            try:
                mdlength = getMetadataLength(self.fd)
                if (mdlength - md['offset']) == \
                    self.VDI_INFO_SIZE_IN_SECTORS * SECTOR_SIZE:
                    updateLengthInHeader(self.fd, (mdlength - \
                                    self.VDI_INFO_SIZE_IN_SECTORS * SECTOR_SIZE))
            except:
                raise
        except Exception, e:
            raise Exception("VDI delete operation failed for "\
                                "parameters: %s, %s. Error: %s" % \
                                (self.path, vdi_uuid, str(e)))

    # common functions with some details derived from the child class
    def generateVDIsForRange(self, vdi_info, lower, upper, update_map = {}, \
                             offset = 0):
        value = ''
        if not len(vdi_info.keys()) or not vdi_info.has_key(offset):
            return self.getVdiInfo(update_map)
            
        for vdi_offset in vdi_info.keys():
            if vdi_offset < lower:
                continue
                    
            if len(value) >= (upper - lower):
                break
            
            vdi_map = vdi_info[vdi_offset]
            if vdi_offset == offset:
                # write passed in VDI info
                for key in update_map.keys():
                    vdi_map[key] = update_map[key]
                        
            for i in range(1, self.VDI_INFO_SIZE_IN_SECTORS + 1):
                if len(value) < (upper - lower):
                    value += self.getVdiInfo(vdi_map, i)
                    
        return value

    def addVdiInternal(self, Dict):
        util.SMlog("Entering addVdiInternal")
        try:
            value = ''
            Dict[VDI_DELETED_TAG] = '0'
            min_block_size = get_min_blk_size_wrapper(self.fd)
            mdlength = getMetadataLength(self.fd)
            md = self.getMetadataInternal({'firstDeleted': 1, 'includeDeletedVdis': 1})
            if not md.has_key('foundDeleted'):
                md['offset'] = mdlength
                (md['lower'], md['upper']) = \
                    getBlockAlignedRange(min_block_size, mdlength, \
                                        SECTOR_SIZE * self.VDI_INFO_SIZE_IN_SECTORS)
            # If this has created a new VDI, update metadata length 
            if md.has_key('foundDeleted'):
                value = self.getMetadataToWrite(md['sr_info'], md['vdi_info'], \
                        md['lower'], md['upper'], Dict, md['offset'])
            else:
                value = self.getMetadataToWrite(md['sr_info'], md['vdi_info'], \
                        md['lower'], md['upper'], Dict, mdlength)
            
            xs_file_write_wrapper(self.fd, md['lower'], min_block_size, \
                                  value, len(value))
            
            if md.has_key('foundDeleted'):
                updateLengthInHeader(self.fd, mdlength)
            else:
                updateLengthInHeader(self.fd, mdlength + \
                        SECTOR_SIZE * self.VDI_INFO_SIZE_IN_SECTORS)
            return True
        except Exception, e:
            util.SMlog("Exception adding vdi with info: %s. Error: %s" % \
                       (Dict, str(e)))
            raise

        
    # Get metadata from the file name passed in
    # additional params:
    # includeDeletedVdis - include deleted VDIs in the returned metadata
    # vdi_uuid - only fetch metadata till a particular VDI
    # offset - only fetch metadata till a particular offset
    # firstDeleted - get the first deleted VDI
    # indexByUuid - index VDIs by uuid
    # the return value of this function is a dictionary having the following keys
    # sr_info: dictionary containing sr information
    # vdi_info: dictionary containing vdi information indexed by offset
    # offset: when passing in vdi_uuid/firstDeleted below
    # deleted - true if deleted VDI found to be replaced
    def getMetadataInternal(self, params = {}):
        try:
            lower = 0; upper = 0
            retmap = {}; sr_info_map = {}; ret_vdi_info = {}
            length = getMetadataLength(self.fd)
            min_blk_size = get_min_blk_size_wrapper(self.fd)
           
            # Read in the metadata fil
            metadataxml = ''
            metadataxml = xs_file_read_wrapper(self.fd, 0, length, min_blk_size)
           
            # At this point we have the complete metadata in metadataxml
            offset = SECTOR_SIZE + len(XML_HEADER)
            sr_info = metadataxml[offset: SECTOR_SIZE * 4]
            offset = SECTOR_SIZE * 4
            sr_info = sr_info.replace('\x00','')
           
            parsable_metadata = '%s<%s>%s</%s>' % (XML_HEADER, metadata.XML_TAG, 
                                                   sr_info, metadata.XML_TAG)
            retmap['sr_info'] = metadata._parseXML(parsable_metadata)
            
            # At this point we check if an offset has been passed in
            if params.has_key('offset'):
                upper = getBlockAlignedRange(min_blk_size, params['offset'], \
                                             0)[1]
            else:
                upper = length
            
            # Now look at the VDI objects
            while offset < upper:
                vdi_info = metadataxml[offset: 
                                offset + 
                                (SECTOR_SIZE * self.VDI_INFO_SIZE_IN_SECTORS)]
                vdi_info = vdi_info.replace('\x00','')
                parsable_metadata = '%s<%s>%s</%s>' % (XML_HEADER, metadata.XML_TAG, 
                                               vdi_info, metadata.XML_TAG)
                vdi_info_map = metadata._parseXML(parsable_metadata)[VDI_TAG]
                vdi_info_map[OFFSET_TAG] = offset
                
                if not params.has_key('includeDeletedVdis') and \
                    vdi_info_map[VDI_DELETED_TAG] == '1':
                    offset += SECTOR_SIZE * self.VDI_INFO_SIZE_IN_SECTORS
                    continue
                
                if params.has_key('indexByUuid'):
                    ret_vdi_info[vdi_info_map[UUID_TAG]] = vdi_info_map
                else:
                    ret_vdi_info[offset] = vdi_info_map

                if params.has_key('vdi_uuid'):
                    if vdi_info_map[UUID_TAG] == params['vdi_uuid']:
                        retmap['offset'] = offset
                        (lower, upper) = \
                            getBlockAlignedRange(min_blk_size, offset, \
                                        SECTOR_SIZE * self.VDI_INFO_SIZE_IN_SECTORS)
                    
                elif params.has_key('firstDeleted'):
                    if vdi_info_map[VDI_DELETED_TAG] == '1':
                        retmap['foundDeleted'] = 1
                        retmap['offset'] = offset
                        (lower, upper) = \
                            getBlockAlignedRange(min_blk_size, offset, \
                                        SECTOR_SIZE * self.VDI_INFO_SIZE_IN_SECTORS)
                            
                offset += SECTOR_SIZE * self.VDI_INFO_SIZE_IN_SECTORS
                
            retmap['lower'] = lower
            retmap['upper'] = upper
            retmap['vdi_info'] = ret_vdi_info
            return retmap
        except Exception, e:
            util.SMlog("Exception getting metadata with params" \
                    "%s. Error: %s" % (params, str(e)))
            raise

    # This function expects both sr name_label and sr name_description to be
    # passed in
    def updateSR(self, Dict):
        util.SMlog('entering updateSR')
        
        value = ''
           
        # Find the offset depending on what we are updating
        diff = set(Dict.keys()) - set(ATOMIC_UPDATE_PARAMS_AND_OFFSET.keys())
        if diff == set([]):
            offset = SECTOR_SIZE * 2       
            (lower, upper) = getBlockAlignedRange(get_min_blk_size_wrapper( \
                self.fd), offset, SECTOR_SIZE * 2)
            md = self.getMetadataInternal({'offset': \
                                SECTOR_SIZE * (SR_INFO_SIZE_IN_SECTORS - 1)})
            
            sr_info = md['sr_info']
            vdi_info_by_offset = md['vdi_info']
           
            # update SR info with Dict
            for key in Dict.keys():
                sr_info[key] = Dict[key]
               
            # if lower is less than SR header size
            if lower < SR_INFO_SIZE_IN_SECTORS * SECTOR_SIZE:
                # if upper is less than SR header size
                if upper <= SR_INFO_SIZE_IN_SECTORS * SECTOR_SIZE:
                    for i in range(lower/SECTOR_SIZE, upper/SECTOR_SIZE):
                        value += self.getSRInfoForSectors(sr_info, range(i, i + 1))
                else:
                    for i in range(lower/SECTOR_SIZE, SR_INFO_SIZE_IN_SECTORS):
                        value += self.getSRInfoForSectors(sr_info, range(i, i + 1))
                   
                    # generate the remaining VDI
                    value += self.generateVDIsForRange(vdi_info_by_offset, 
                                SR_INFO_SIZE_IN_SECTORS, upper)
            else:
                # generate the remaining VDI
                value += self.generateVDIsForRange(vdi_info_by_offset, lower, upper)
            
            xs_file_write_wrapper(self.fd, lower, \
                get_min_blk_size_wrapper(self.fd), value, len(value))
        else:
            raise Exception("SR Update operation not supported for "
                            "parameters: %s" % diff)
    
    def updateVdi(self, Dict):
        util.SMlog('entering updateVdi')
        try:
            value = ''
            min_block_size = get_min_blk_size_wrapper(self.fd)
            mdlength = getMetadataLength(self.fd)
            md = self.getMetadataInternal({'vdi_uuid': Dict[UUID_TAG]})
            value = self.getMetadataToWrite(md['sr_info'], md['vdi_info'], \
                        md['lower'], md['upper'], Dict, md['offset'])
            xs_file_write_wrapper(self.fd, md['lower'], min_block_size, value, len(value))
            return True
        except Exception, e:
            util.SMlog("Exception updating vdi with info: %s. Error: %s" % \
                       (Dict, str(e)))
            raise
            
    # This should be called only in the cases where we are initially writing
    # metadata, the function would expect a dictionary which had all information
    # about the SRs and all its VDIs
    def writeMetadataInternal(self, sr_info, vdi_info):
        try:
            md = ''
            md = self.getSRInfoForSectors(sr_info, range(0, SR_INFO_SIZE_IN_SECTORS))
           
            # Go over the VDIs passed and for each
            for key in vdi_info.keys():
                md += self.getVdiInfo(vdi_info[key])
           
            # Now write the metadata on disk.
            min_block_size = get_min_blk_size_wrapper(self.fd)
            xs_file_write_wrapper(self.fd, 0, min_block_size, md, len(md))
            updateLengthInHeader(self.fd, len(md))
           
        except Exception, e:
            util.SMlog("Exception writing metadata with info: %s, %s. "\
                       "Error: %s" % (sr_info, vdi_info, str(e)))
            raise

    # generates metadata info to write taking the following parameters:
    # a range, lower - upper
    # sr and vdi information
    # VDI information to update
    # an optional offset to the VDI to update
    def getMetadataToWrite(self, sr_info, vdi_info, lower, upper, update_map, \
                           offset):
        util.SMlog("Entering getMetadataToWrite")
        try:
            value = ''
            vdi_map = {}
            
            # if lower is less than SR info
            if lower < SECTOR_SIZE * SR_INFO_SIZE_IN_SECTORS:
                # generate SR info
                for i in range(lower/SECTOR_SIZE, SR_INFO_SIZE_IN_SECTORS):
                    value += self.getSRInfoForSectors(sr_info, range(i, i + 1))
                
                # generate the rest of the VDIs till upper
                value += self.generateVDIsForRange(vdi_info, \
                   SECTOR_SIZE * SR_INFO_SIZE_IN_SECTORS, upper, update_map, offset)
            else:
                # skip till you get a VDI with lower as the offset, then generate
                value += self.generateVDIsForRange(vdi_info, lower, upper, \
                                              update_map, offset)
            return value
        except Exception, e:
            util.SMlog("Exception generating metadata to write with info: "\
                       "sr_info: %s, vdi_info: %s, lower: %d, upper: %d, "\
                       "update_map: %s, offset: %d. Error: %s" % \
                       (sr_info, vdi_info, lower, upper, update_map, offset,str(e)))
            raise

    # specific functions, to be implement by the child classes
    def getVdiInfo(self, Dict, generateSector = 0):
        return
    
    def getSRInfoForSectors(self, sr_info, range):
        return 

class LVMMetadataHandler(MetadataHandler):
    
    VDI_INFO_SIZE_IN_SECTORS = 2
    
    # constructor
    def __init__(self, path = None, write = True):
        lvutil.ensurePathExists(path)
        MetadataHandler.__init__(self, path, write)
    
    def spaceAvailableForVdis(self, count):
        try:
            created = False
            try:
                # The easiest way to do this, is to create a dummy vdi and write it
                uuid = util.gen_uuid()
                vdi_info = { UUID_TAG: uuid,
                            NAME_LABEL_TAG: 'dummy vdi for space check',
                            NAME_DESCRIPTION_TAG: 'dummy vdi for space check',
                            IS_A_SNAPSHOT_TAG: 0,
                            SNAPSHOT_OF_TAG: '',
                            SNAPSHOT_TIME_TAG: '',
                            TYPE_TAG: 'user',
                            VDI_TYPE_TAG: 'vhd',
                            READ_ONLY_TAG: 0,
                            MANAGED_TAG: 0,
                            'metadata_of_pool': ''
                }
       
                created = self.addVdiInternal(vdi_info)
            except IOError, e:
                raise      
        finally:
            if created:
                # Now delete the dummy VDI created above
                self.deleteVdi(uuid)
                return

    # This function generates VDI info based on the passed in information
    # it also takes in a parameter to determine whether both the sector
    # or only one sector needs to be generated, and which one
    # generateSector - can be 1 or 2, defaults to 0 and generates both sectors
    def getVdiInfo(self, Dict, generateSector = 0):
        util.SMlog("Entering VDI info")
        try:
            vdi_info = ''
            # HP split into 2 functions, 1 for generating the first 2 sectors,
            # which will be called by all classes
            # and one specific to this class
            if generateSector == 1 or generateSector == 0:
                Dict[NAME_LABEL_TAG] = \
                        xml.sax.saxutils.escape(Dict[NAME_LABEL_TAG])
                Dict[NAME_DESCRIPTION_TAG] = \
                        xml.sax.saxutils.escape(Dict[NAME_DESCRIPTION_TAG])
                if len(Dict[NAME_LABEL_TAG]) + len(Dict[NAME_DESCRIPTION_TAG]) > \
                    MAX_VDI_NAME_LABEL_DESC_LENGTH:
                    if len(Dict[NAME_LABEL_TAG]) > MAX_VDI_NAME_LABEL_DESC_LENGTH/2:
                        length = util.unictrunc(Dict[NAME_LABEL_TAG], \
                                MAX_VDI_NAME_LABEL_DESC_LENGTH/2)

                        util.SMlog('warning: name-label truncated from ' \
                                + str(len(Dict[NAME_LABEL_TAG])) + ' to ' \
                                + str(length) + ' bytes')

                        Dict[NAME_LABEL_TAG] = Dict[NAME_LABEL_TAG][:length]
                   
                    if len(Dict[NAME_DESCRIPTION_TAG]) > \
                        MAX_VDI_NAME_LABEL_DESC_LENGTH/2: \

                        length = util.unictrunc(Dict[NAME_DESCRIPTION_TAG], \
                                MAX_VDI_NAME_LABEL_DESC_LENGTH/2)

                        util.SMlog('warning: description truncated from ' \
                            + str(len(Dict[NAME_DESCRIPTION_TAG])) + \
                            ' to ' + str(length) + ' bytes')

                        Dict[NAME_DESCRIPTION_TAG] = \
                                Dict[NAME_DESCRIPTION_TAG][:length]
                       
                # Fill the open struct and write it
                vdi_info += getSector(VDI_SECTOR_1 % (Dict[NAME_LABEL_TAG], 
                                                      Dict[NAME_DESCRIPTION_TAG]))
           
            if generateSector == 2 or generateSector == 0:
                sector2 = ''
                
                if not Dict.has_key(VDI_DELETED_TAG):
                    Dict.update({VDI_DELETED_TAG:'0'})
                
                for tag in Dict.keys():
                    if tag == NAME_LABEL_TAG or tag == NAME_DESCRIPTION_TAG:
                        continue
                    sector2 += getXMLTag(tag) % Dict[tag]
                    
                sector2 += VDI_CLOSING_TAG
                vdi_info += getSector(sector2)
            return vdi_info
       
        except Exception, e:
            util.SMlog("Exception generating vdi info: %s. Error: %s" % \
                       (Dict, str(e)))
            raise
    
    def getSRInfoForSectors(self, sr_info, range):
        srinfo = ''
        
        try:
            # write header, name_labael and description in that function
            # as its common to all
            # Fill up the first sector 
            if 0 in range:
                srinfo = getSector(buildHeader(SECTOR_SIZE))
               
            if 1 in range:
                uuid = getXMLTag(UUID_TAG) % sr_info[UUID_TAG]
                allocation = getXMLTag(ALLOCATION_TAG) % sr_info[ALLOCATION_TAG]
                
                second = SECTOR2_FMT % (XML_HEADER, uuid, allocation)
                srinfo += getSector(second)
           
            if 2 in range:
                # Fill up the SR name_label
                srinfo += getSector(getSectorAlignedXML(NAME_LABEL_TAG, 
                    xml.sax.saxutils.escape(sr_info[NAME_LABEL_TAG])))
               
            if 3 in range:
                # Fill the name_description
                srinfo += getSector(getSectorAlignedXML(NAME_DESCRIPTION_TAG, 
                    xml.sax.saxutils.escape(sr_info[NAME_DESCRIPTION_TAG])))
            
            return srinfo
        
        except Exception, e:
            util.SMlog("Exception getting SR info with parameters: sr_info: %s," \
                       "range: %s. Error: %s" % (sr_info, range, str(e)))
            raise
       
class SLMetadataHandler(MetadataHandler):
    
    VDI_INFO_SIZE_IN_SECTORS = 2
    SECTOR2_FMT= "%s%s"
    
    # constructor
    def __init__(self, path = None, write = True):
        MetadataHandler.__init__(self, path, write)
    
    def spaceAvailableForVdis(self, count):
        try:
            created = False
            try:
                # The easiest way to do this, is to create a dummy vdi and write it
                uuid = util.gen_uuid()
                vdi_info = { UUID_TAG: uuid,
                            NAME_LABEL_TAG: 'dummy vdi for space check',
                            NAME_DESCRIPTION_TAG: 'dummy vdi for space check',
                            IS_A_SNAPSHOT_TAG: 0,
                            SNAPSHOT_OF_TAG: '',
                            SNAPSHOT_TIME_TAG: '',
                            TYPE_TAG: 'user',
                            VDI_TYPE_TAG: 'vhd',
                            READ_ONLY_TAG: 0,
                            MANAGED_TAG: 0,
                            'metadata_of_pool': ''
                }
       
                created = self.addVdiInternal(vdi_info)
            except IOError, e:
                raise      
        finally:
            if created:
                # Now delete the dummy VDI created above
                self.deleteVdi(uuid)
                return

    # This function generates VDI info based on the passed in information
    # it also takes in a parameter to determine whether both the sector
    # or only one sector needs to be generated, and which one
    # generateSector - can be 1 or 2, defaults to 0 and generates both sectors
    def getVdiInfo(self, Dict, generateSector = 0):
        util.SMlog("Entering VDI info")
        try:
            vdi_info = ''
            # HP split into 2 functions, 1 for generating the first 2 sectors,
            # which will be called by all classes
            # and one specific to this class
            if generateSector == 1 or generateSector == 0:
                Dict[NAME_LABEL_TAG] = \
                        xml.sax.saxutils.escape(Dict[NAME_LABEL_TAG])
                Dict[NAME_DESCRIPTION_TAG] = \
                        xml.sax.saxutils.escape(Dict[NAME_DESCRIPTION_TAG])
                if len(Dict[NAME_LABEL_TAG]) + len(Dict[NAME_DESCRIPTION_TAG]) > \
                    MAX_VDI_NAME_LABEL_DESC_LENGTH:
                    if len(Dict[NAME_LABEL_TAG]) > MAX_VDI_NAME_LABEL_DESC_LENGTH/2:
                        length = util.unictrunc(Dict[NAME_LABEL_TAG], \
                                MAX_VDI_NAME_LABEL_DESC_LENGTH/2)

                        util.SMlog('warning: name-label truncated from ' \
                                + str(len(Dict[NAME_LABEL_TAG])) + ' to ' \
                                + str(length) + ' bytes')

                        Dict[NAME_LABEL_TAG] = Dict[NAME_LABEL_TAG][:length]
                   
                    if len(Dict[NAME_DESCRIPTION_TAG]) > \
                        MAX_VDI_NAME_LABEL_DESC_LENGTH/2: \

                        length = util.unictrunc(Dict[NAME_DESCRIPTION_TAG], \
                                MAX_VDI_NAME_LABEL_DESC_LENGTH/2)

                        util.SMlog('warning: description truncated from ' \
                            + str(len(Dict[NAME_DESCRIPTION_TAG])) + \
                            ' to ' + str(length) + ' bytes')

                        Dict[NAME_DESCRIPTION_TAG] = \
                                Dict[NAME_DESCRIPTION_TAG][:length]
                       
                # Fill the open struct and write it
                vdi_info += getSector(VDI_SECTOR_1 % (Dict[NAME_LABEL_TAG], 
                                                      Dict[NAME_DESCRIPTION_TAG]))
           
            if generateSector == 2 or generateSector == 0:
                sector2 = ''
                
                if not Dict.has_key(VDI_DELETED_TAG):
                    Dict.update({VDI_DELETED_TAG:'0'})
                
                for tag in Dict.keys():
                    if tag == NAME_LABEL_TAG or tag == NAME_DESCRIPTION_TAG:
                        continue
                    sector2 += getXMLTag(tag) % Dict[tag]
                    
                sector2 += VDI_CLOSING_TAG
                vdi_info += getSector(sector2)
            return vdi_info
       
        except Exception, e:
            util.SMlog("Exception generating vdi info: %s. Error: %s" % \
                       (Dict, str(e)))
            raise
    
    def getSRInfoForSectors(self, sr_info, range):
        srinfo = ''
        
        try:
            # write header, name_labael and description in that function
            # as its common to all
            # Fill up the first sector 
            if 0 in range:
                srinfo = getSector(buildHeader(SECTOR_SIZE))
               
            if 1 in range:
                uuid = getXMLTag(UUID_TAG) % sr_info[UUID_TAG]                
                second = self.SECTOR2_FMT % (XML_HEADER, uuid)
                srinfo += getSector(second)
           
            if 2 in range:
                # Fill up the SR name_label
                srinfo += getSector(getSectorAlignedXML(NAME_LABEL_TAG, 
                    xml.sax.saxutils.escape(sr_info[NAME_LABEL_TAG])))
               
            if 3 in range:
                # Fill the name_description
                srinfo += getSector(getSectorAlignedXML(NAME_DESCRIPTION_TAG, 
                    xml.sax.saxutils.escape(sr_info[NAME_DESCRIPTION_TAG])))
            
            return srinfo
        
        except Exception, e:
            util.SMlog("Exception getting SR info with parameters: sr_info: %s," \
                       "range: %s. Error: %s" % (sr_info, range, str(e)))
            raise
       
