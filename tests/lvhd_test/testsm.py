#!/usr/bin/python

import re
import os
import sys
import time
import getopt
import traceback
import random

import tutil
import storagemanager

sys.path.append("/opt/xensource/sm")
from ipc import IPCFlag
import LVHDSR
import lvhdutil
import util

CREATE_SIZES = [
        1 * tutil.MiB, 
        10 * tutil.MiB,
        100 * tutil.MiB,
        1024 * tutil.MiB,
        10 * tutil.GiB ]

SNAPSHOT_NUM_ITERS = 4
SNAPSHOT_SIZES = [
        10 * tutil.MiB,
        10 * tutil.GiB ]

RESIZE_START_SIZE = 10 * tutil.MiB
RESIZE_SIZES = [
        17 * tutil.MiB,
         3 * tutil.GiB,
        10 * tutil.GiB ]
RESIZE_SNAPSHOTS = 3
RESIZE_PREFILL_SIZES = [
        512,
        6 * tutil.MiB ]

VDI_SIZE_DEFAULT = 1 * tutil.GiB
COALESCE_RND_SIZE = 10 * tutil.GiB
COALESCE_RND_ITERS = 4
COALESCE_RND_WFRAQ = 0.01

# don't run disktest in "basic" mode on VDI's larger than this size (to save 
# time)
MAX_DISKTEST_SIZE = 1 * tutil.GiB

LOG_FILE = "/tmp/testsm.log"

VHD_SIZE_INCREMENT = (2 * tutil.MiB)
LVM_SIZE_INCREMENT = (4 * tutil.MiB)

MGT_LV_NAME = "MGT"
MGT_LV_SIZE = LVM_SIZE_INCREMENT

NUM_RETRIES = 300
RETRY_PERIOD = 3

RESIZE_FIST_POINTS = [
        "vhd_fail_resize_begin",
        "vhd_fail_resize_data",
        "vhd_fail_resize_metadata",
        "vhd_fail_resize_end" ]

SNAPSHOT_FIST_POINTS = { # map the FIST points to whether the op succeeds
        "LVHDRT_clone_vdi_after_create_journal": False,
        "LVHDRT_clone_vdi_after_shrink_parent": False,
        "LVHDRT_clone_vdi_after_lvcreate": False,
        "LVHDRT_clone_vdi_after_first_snap": False,
        "LVHDRT_clone_vdi_after_second_snap": True,
        "LVHDRT_clone_vdi_after_parent_hidden": True,
        "LVHDRT_clone_vdi_after_parent_ro": True,
        "LVHDRT_clone_vdi_before_remove_journal": True }

SNAPSHOT_RECOVERY_FIST_POINTS = [
        "LVHDRT_clone_vdi_before_undo_clone",
        "LVHDRT_clone_vdi_after_undo_clone" ]

COALEAF_FIST_POINTS = { # map the FIST points to whether the coalesce succeeds
        "LVHDRT_coaleaf_before_coalesce": False,
        "LVHDRT_coaleaf_after_coalesce": False,
        "LVHDRT_coaleaf_one_renamed": False,
        "LVHDRT_coaleaf_both_renamed": False,
        "LVHDRT_coaleaf_after_vdirec": False,
        "LVHDRT_coaleaf_before_delete": False,
        "LVHDRT_coaleaf_after_delete": True,
        "LVHDRT_coaleaf_before_remove_j": True }

COALEAF_RECOVERY_FIST_POINTS = [
        "LVHDRT_coaleaf_undo_after_rename",
        "LVHDRT_coaleaf_undo_after_rename2",
        "LVHDRT_coaleaf_undo_after_refcount",
        "LVHDRT_coaleaf_undo_after_deflate",
        "LVHDRT_coaleaf_undo_end",
        "LVHDRT_coaleaf_finish_after_inflate",
        "LVHDRT_coaleaf_finish_end" ]

COALEAF_CONCURRENCY_FIST_POINTS = [
        "LVHDRT_coaleaf_delay_1",
        "LVHDRT_coaleaf_delay_2",
        "LVHDRT_coaleaf_delay_3" ]

logger = None
vmDom0 = { "master": None, "slave": None }
thisHost = "master"
srType = ""
testMode = ""
testNum = 0
skipTo = 0
stopAfter = 100000

sm = None

FULL_BLOCK_SIZE = 2 * tutil.MiB
PART_BLOCK_SIZE = 15 * tutil.KiB
CHAR_SEQ = "".join([chr(x) for x in range(256)])
CHAR_SEQ_REV = "".join([chr(x) for x in range(255, -1, -1)])
BUF_PATTERN = CHAR_SEQ + CHAR_SEQ
BUF_PATTERN_REV = CHAR_SEQ_REV + CHAR_SEQ_REV
BUF_ZEROS = "\0" * 512

class TestException(Exception):
    pass

class StopRequest(Exception):
    pass

def assertEqual(val1, val2):
    assertRel("eq", val1, val2)

def assertRel(rel, val1, val2):
    if rel == "eq":
        if val1 != val2:
            raise TestException("%s != %s" % (val1, val2))
    elif rel == "ne":
        if val1 == val2:
            raise TestException("%s == %s" % (val1, val2))
    elif rel == "gt":
        if val1 <= val2:
            raise TestException("%s <= %s" % (val1, val2))
    elif rel == "ge":
        if val1 < val2:
            raise TestException("%s < %s" % (val1, val2))
    elif rel == "lt":
        if val1 >= val2:
            raise TestException("%s >= %s" % (val1, val2))
    elif rel == "le":
        if val1 > val2:
            raise TestException("%s > %s" % (val1, val2))
    else:
        raise TestException("Invalid rel: %s" % rel)

def setFistPoint(point, active):
    args = {"fistPoint": point, "active": active}
    ret = sm.onMaster("testing-hooks", "setFistPoint", args)
    if ret != "True":
        raise TestException("ERROR setting FIST point %s to %s" % \
                (point, active))

def stop(msg = None):
    """Call this function to stop the test suite at any chosen point"""
    text = ""
    if msg:
        text = msg
    raise StopRequest(text)

def pause(msg = None):
    """Call this function to pause the test suite at any chosen point and wait
    for the user to press enter"""
    if msg:
        logger.log("\n\n* * * %s: Press Enter to continue..." % msg, 0)
    else:
        logger.log("\n\n* * * Pausing test, press enter to continue...", 0)
    garbage = raw_input()

def _disableCommand(cmd, success):
    if not tutil.pathExists(cmd):
        raise TestException("Command not found: %s" % cmd)
    if success:
        cmdFake = "/bin/true"
    else:
        cmdFake = "/bin/false"
    cmdBackup = "%s.orig" % cmd
    if not tutil.pathExists(cmdBackup):
        os.rename(cmd, cmdBackup)
    os.unlink(cmd)
    os.symlink(cmdFake, cmd)

def _enableCommand(cmd):
    cmdBackup = "%s.orig" % cmd
    if not tutil.pathExists(cmdBackup):
        raise TestException("Command had not been disabled: %s" % cmd)
    os.unlink(cmd)
    os.symlink(cmdBackup, cmd)

def _nextTest(description, incremement = True):
    """Tells whether the next test should be executed."""
    global testNum
    text = "[  -] "
    if incremement:
        testNum += 1
        text = "[%3d] " % testNum
    if testNum >= stopAfter + 1:
        stop("After test %d" % (testNum - 1))
    run = testNum >= skipTo
    if not run:
        text = "[skipping] " + text
    logger.log(text + description, 0)
    return run

def _findTargetSR(srType):
    """Retrieves the UUID of any empty SR of the specified type."""

    sr = None
    srList = sm.findSR(srType)
    if len(srList) == 0:
        raise TestException("No SR of type '%s' found" % srType)
    elif len(srList) == 1:
        sr = srList[0]
    elif len(srList) > 1:
        sr = sm.getDefaultSR(sm)
        # If the SM thinks that this SR is of different type (TODO how is this
        # possible?), pick any other SR that has a PDB. TODO verify comment
        if sm.srTypes[sr] != srType:
            sr = None
            for candidate in srList:
                if len(sm.findThisPBD(candidate)) > 0:
                    sr = candidate
                    break
        if not sr:
            raise TestException("No SRs of type %s found on this host" % srType)
    logger.log("Target SR: %s" % sr, 0)
    if len(sm.getVDIs(sr)) > 0:
        raise TestException("Target SR not empty")
    return sr

def _createSpaceHogFile(sr, allocSize):
    BS = 4 * tutil.KiB
    srDir = sm.getPathSR(sr)
    fn = "%s/space.hog" % srDir
    numBlocks = allocSize / BS
    logger.log("Creating a space hog file %s (%d blocks)" % (fn, numBlocks), 2)
    f = open(fn, 'w')
    for i in range(numBlocks):
        f.seek(i * BS)
        f.write(BUF_ZEROS)
    f.close()

def _destroySpaceHogFile(sr):
    srDir = sm.getPathSR(sr)
    fn = "%s/space.hog" % srDir
    os.unlink(fn)

def leaveFreeSpace(sr, space):
    info = sm.getInfoSR(sr)
    totalSize = int(info["physical-size"])
    currUtiln = int(info["physical-utilisation"])
    allocSize = totalSize - currUtiln - space
    assert(allocSize > 0)
    if srType == "lvhd":
        vdi = sm.createVDI(sr, allocSize, True)
    else:
        _createSpaceHogFile(sr, allocSize)
    sm.refreshSR(sr)
    info = sm.getInfoSR(sr)
    totalSize = int(info["physical-size"])
    currUtiln = int(info["physical-utilisation"])
    freeSpace = totalSize - currUtiln
    logger.log("Left free space in SR: %d (asked: %d)" % (freeSpace, space), 2)
    return freeSpace

def getNumFiles(sr):
    if srType == "lvhd":
        cmd = "lvs VG_XenStorage-%s --noheadings | grep -v %s | wc -l" % \
                (sr, MGT_LV_NAME)
        text = tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_SUB)
        return int(text) 
    else:
        files = os.listdir("/var/run/sr-mount/%s" % sr)
        num = 0
        for file in files:
            if file.endswith(".vhd"):
                num += 1
        return num

def validateVHD(sr, vdi, live):
    """Checks that the specified VDI is in good state. If the VDI is not in
    good state, an exception is raised."""

    vdis = sm.activateChain(sr, vdi)
    path = sm.getPathVDI(sr, vdi)
    cmd = "vhd-util check -n %s" % path
    if live:
        cmd += " -i"
    tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_CMD + 1)
    sm.deactivateVDIs(sr, vdis)

def checkSize(path, size):
    # stat # Note: stat doesn't work on block devices for some reason
    #statinfo = os.stat(path)
    #statSize = statinfo.st_size
    #if size != statSize:
    #    raise TestException("stat size (%s) %d != %d (expected)" % \
    #            (statinfo, statSize, size))

    # sysfs
    devName = path.split("/")[-1]
    f = open("/sys/block/%s/size" % devName, 'r')
    line = f.readline().strip()
    sysfsSize = int(line) * 512
    if size != sysfsSize:
        raise TestException("sysfs size %d != %d (expected)" % \
                (sysfsSize, size))

    # fdisk
    fdiskSize = -1
    cmd = "fdisk -l %s" % path
    stdout = tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_CMD)
    for line in stdout.split('\n'):
        m = re.match("^Disk [^,]+, (\d+) bytes", line)
        if m != None:
            fdiskSize = int(m.group(1))
    if size != fdiskSize:
        raise TestException("fdisk reported size %d != %d (expected)" % \
                (fdiskSize, size))

def _waitForGC(sr, numFiles, retries = NUM_RETRIES):
    """wait util there are at most numFiles LV's left"""
    kicked = False
    prevNum = 0
    i = 0
    while i < retries:
        num = getNumFiles(sr)
        logger.log("Waiting for GC to finish leave %d... (curr: %d, iter %d)" % \
                (numFiles, num, i), 3)
        if num <= numFiles:
            return
        if num < prevNum:
            i = 0 # only time out if GC is not making progress
            logger.log("Giving GC more time", 3)
        else:
            i += 1
            if i >= NUM_RETRIES / 2 and not kicked:
                logger.log("Kicking GC just in case")
                sm.refreshSR(sr)
                kicked = True
        prevNum = num
        time.sleep(RETRY_PERIOD)
    raise TestException("Timed out waiting for %d LV's to be left" % numFiles)

def _getThinProvisionFlag(srInfo):
    """Tells whether thin provisioning is enabled by looking into the SR
    parameters."""
    return (srInfo["sm-config"].get("allocation") and \
            srInfo["sm-config"]["allocation"] == "thin")

def _testAttachVDI(vdi, ro, vm = None):
    """ Attaches the specified VDI."""

    if not vm:
        vm = vmDom0[thisHost]
    sm.plugVDI(vdi, ro, vm)
    if vm == vmDom0[thisHost]:
        if not sm.getDevice(vdi):
            raise TestException("Failed to plug VDI %s" % vdi)
        info = sm.getInfoVDI(vdi)
        virtSize = int(info["virtual-size"])
        checkSize(_getDevice(vdi), virtSize)

def _getDevice(vdi):
    device = sm.getDevice(vdi)
    if not device:
        raise TestException("Device for VDI %s not found" % vdi)
    return device

def _testDetachVDI(vdi):
    sm.unplugVDI(vdi)

def _testCreateVDI(sr, size, raw):
    """Creates a VDI on the specified SR with the specified size."""

    info = sm.getInfoSR(sr)
    srUtilisationBefore = int(info["physical-utilisation"])
    thinProvision = _getThinProvisionFlag(info)

    # Create the VDI.
    vdi = sm.createVDI(sr, size, raw)
    if not raw:
        validateVHD(sr, vdi, False)

    vdis = sm.getLeafVDIs(sr)

    if len(vdis) != len(sm.createdVDIs):
        raise TestException("Incorrect number of VDIs: %d (expected %d)" % \
                (len(vdis), len(sm.createdVDIs)))

    if not vdi in vdis:
        raise TestException("VDI not found in SR listing: %s != %s" % \
                (vdi, vdis[0]))

    info = sm.getInfoVDI(vdi)
    virtSize = int(info["virtual-size"])
    increment = VHD_SIZE_INCREMENT
    if raw:
        increment = LVM_SIZE_INCREMENT
    if size % increment:
        size = (size / increment + 1) * increment
    if size != virtSize:
        raise TestException("Virtual size doesn't match: requested=%d, got=%d"\
                % (size, virtSize))

    utilisation = int(info["physical-utilisation"])
    # Ensure the size of the VDI is correct.
    if raw:
        if utilisation != size:
            raise TestException("Physical utilisation invalid for RAW: %d" % \
                    utilisation)
    else:
        overhead = sm.getOverheadVHDEmpty(size)
        if srType == "lvhd":
            overhead = sm.getSizeLV(sm.getOverheadVHDEmpty(lvhdutil.MSIZE))
        expectedUtiln = overhead
        if srType == "lvhd":
            if not thinProvision:
                expectedUtiln = sm.getVHDLVSize(size)
            expectedUtiln = sm.getSizeLV(expectedUtiln)
        if (srType == "lvhd" and utilisation != expectedUtiln) or \
                (utilisation > expectedUtiln):
            raise TestException("Physical utilisation %d != %d (expected)" % \
                    (utilisation, expectedUtiln))

    # this test fails because SR utilisation isn't updated immediately
    #info = sm.getInfoSR(sr)
    #srUtilisationAfter = int(info["physical-utilisation"])
    #if srUtilisationAfter - srUtilisationBefore != utilisation:
    #    raise TestException("Incorrect SR utilisation: %d (expected: %d)" % \
    #            (srUtilisationAfter - srUtilisationBefore, utilisation))
    return vdi

def _testSnapshotVDI(sr, vdi, live, single = False):
    vdiSnap = sm.snapshotVDI(vdi, single)
    info = sm.getInfoVDI(vdiSnap)
    logger.log("snap info: %s" % info, 4)
    raw = False
    if srType == "lvhd":
        raw = (info["sm-config"]["vdi_type"] == "aio")
    if not (raw and single):
        validateVHD(sr, vdiSnap, live)
    if srType != "lvhd":
        sm.refreshSR(sr)
    virtSize = int(info["virtual-size"])
    utilisation = int(info["physical-utilisation"])
    if single:
        assertEqual(info["managed"], "false")
        assertEqual(info["read-only"], "true")
    else:
        assertEqual(info["managed"], "true")
        assertEqual(info["read-only"], "false")

    overhead = sm.getOverheadVHDEmpty(virtSize)
    if srType == "lvhd":
        overhead = sm.getOverheadVHDEmpty(lvhdutil.MSIZE)
    expectedUtiln = overhead
    #if not _getThinProvisionFlag(sm.getInfoSR(sr)):
    #   expectedUtiln += virtSize
    expectedUtiln = sm.getSizeLV(expectedUtiln)
    if srType == "lvhd":
        if single:
            assertRel("ge", utilisation, expectedUtiln)
        else:
            assertEqual(utilisation, expectedUtiln)
    return vdiSnap

def _checkActiveLVs(sr):
    for i in range(NUM_RETRIES):
        active = False
        proof = ""
        cmd = "lvs VG_XenStorage-%s --noheadings" % sr
        text = tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_CMD)
        for line in text.split('\n'):
            if not line.strip():
                continue
            if line.split()[2][4] == 'a':
                active = True
                proof = line
                break
        if not active:
            break
        logger.log("Waiting for LVs to be deactivated (%d)" % i)
        time.sleep(RETRY_PERIOD)
    if active:
        raise TestException("LV active when nothing attached: %s" % proof)

def _testDestroyAll(sr):
    logger.log("Destroy all VDIs", 1)
    sm.unplugAll()
    if srType == "lvhd":
        _checkActiveLVs(sr)
    sm.destroyAll()
    _waitForGC(sr, 0)
    sm.refreshSR(sr)
    vdis = sm.getVDIs(sr)
    if len(vdis) != 0:
        raise TestException("Non-zero number of VDIs: %d (%s)" % \
                (len(vdis), vdis))
    info = sm.getInfoSR(sr)
    srUtilisation = int(info["physical-utilisation"])
    if srType == "lvhd" and srUtilisation > MGT_LV_SIZE:
        raise TestException("Empty SR utilisation %d != 0" % srUtilisation)

def _testWrite(vdi, size, i, marker = "MARKER"):
    path = sm.getDevice(vdi)
    offset = i * VHD_SIZE_INCREMENT
    if offset + VHD_SIZE_INCREMENT > size:
        return
    logger.log("Writing to '%s', offset %d (i=%d)" % (path, offset, i), 2)
    try:
        f = open(path, "w")
        f.seek(offset)
        f.write("block %d (marker: %s)\n" % (i, marker))
        f.close()
    except IOError:
        raise TestException("Error writing to '%s'" % path)
    _testRead(vdi, size, i, False, marker)

def _testWritePattern(vdi, numBlocks, skipEvery, writeAmount, rev = False):
    path = sm.getDevice(vdi)
    try:
        buf = BUF_PATTERN
        if rev:
            buf = BUF_PATTERN_REV

        f = open(path, "w")
        block = 0
        while block < numBlocks:
            f.seek(block * VHD_SIZE_INCREMENT)
            logger.log("Writing to '%s', block=%d" % (path, block), 3)
            i = 0
            while i < writeAmount:
                f.write(buf)
                i += len(buf)
            block += 1 + skipEvery
        f.close()
    except IOError:
        raise TestException("Error writing to '%s'" % path)

def _testReadPattern(vdi, numBlocks, checkAll, skipEvery, writeAmount,
        allowEmpty = False, rev = False):
    path = sm.getDevice(vdi)
    bufExpected = BUF_PATTERN
    if rev:
        bufExpected = BUF_PATTERN_REV
    readAmount = writeAmount * 3
    if readAmount > FULL_BLOCK_SIZE:
        readAmount = FULL_BLOCK_SIZE
    try:
        f = open(path, "r")
        block = 0
        while block < numBlocks:
            logger.log("Reading '%s' to verify (block %d)" % (path, block), 3)
            f.seek(block * VHD_SIZE_INCREMENT)
            i = 0
            while i < readAmount:
                buf = f.read(len(bufExpected))
                expect = bufExpected
                if block % (skipEvery + 1): # did we write into this block?
                    expect = BUF_ZEROS
                if i >= writeAmount: # did we write at this offset?
                    expect = BUF_ZEROS
                if buf != expect:
                    if allowEmpty and buf == BUF_ZEROS:
                        logger.log("Allowing empty contents at %d+%d" % 
                                (block, i), 0)
                    else:
                        raise Exception("Readback mismatch! block = " + \
                                "%d, off = %d, expected '%s', got '%s'" % \
                                (block, i, expect, buf))
                i += len(buf)
            block += 1
            if not checkAll:
                block *= 2
        f.close()
    except IOError:
        raise TestException("Error reading '%s'" % path)

def _testAttachWrite(vdi, live, size, i, doAttach = False):
    if not live or doAttach:
        _testAttachVDI(vdi, False)
    _testWrite(vdi, size, i)
    if not live:
        _testDetachVDI(vdi)

def _testWriteRandom(vdi, size, fraq, i):
    path = sm.getDevice(vdi)
    seedMultiplier = 181
    cmd = "disktest write %s %s -r -p %d -s %s" % \
            (path, i, int(100 * fraq), i * seedMultiplier)
    text = tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_CMD)

def _getHash(vdi):
    path = sm.getDevice(vdi)
    cmd = "md5sum -b %s" % path
    text = tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_CMD)
    return text

def _testRead(vdi, size, i, all, marker = "MARKER"):
    path = sm.getDevice(vdi)
    #logger.log("Read '%s' to verify (block %d)" % (path, i), 2)
    vals = [i]
    if all:
        vals = range(i + 1)
    try:
        f = open(path, "r")
        for j in vals:
            logger.log("Read '%s' to verify (block %d)" % (path, j), 2)
            f.seek(j * VHD_SIZE_INCREMENT)
            expected = "block %d (marker: %s)\n" % (j, marker)
            text = f.read(len(expected))
            if text != expected:
                raise TestException("Contents mismatch: got: '%s', " \
                        "expected: '%s'" % (text, expected))
        f.close()
    except IOError:
        raise TestException("Error reading '%s'" % path)

def _runDiskTest(vdi):
    path = sm.getDevice(vdi)
    cmd = "disktest write %s 77" % (path)
    tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_CMD)
    cmd = "disktest verify %s 77" % (path)
    tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_CMD)

def _testMultiAttach(vdi, size):
    """Attaches/detaches the specified VDI, writing and reading some data
    between these two operations. The size argument control the amount of data
    read/written."""

    text = "Multiple attaches"
    if not _nextTest(text, False): return

    NUM_SIMULT_VBDS = 1 # TODO
    NUM_BLOCKS = 3
    _testAttachVDI(vdi, False)
    for i in range(NUM_BLOCKS):
        _testWrite(vdi, size, i)
    _testDetachVDI(vdi)
    _testAttachVDI(vdi, True)
    _testRead(vdi, size, NUM_BLOCKS - 1, True)
    _testDetachVDI(vdi)

def _testSnapshot(sr, raw, live, write, size, iters):
    text = "Snapshot of "
    if raw:
        text += "raw VDI, "
    else:
        text += "VHD VDI, "
    if live:
        text += "live, "
    else:
        text += "offline, "
    if write:
        text += "non-"
    text += "empty, size %d (%d times)" % (size, iters)
    if not _nextTest(text): return

    vdi = _testCreateVDI(sr, size, raw)
    if live or write:
        _testAttachVDI(vdi, False)
        if write:
            _testWrite(vdi, size, 0)
        if not live:
            _testDetachVDI(vdi)

    vdiClone = vdi
    for i in range(iters):
        logger.log("(iteration %d of %d)" % (i + 1, iters), 1)
        vdiClone = _testSnapshotVDI(sr, vdiClone, live)
        for ht in ["master", "slave"]:
            if not vmDom0[ht]:
                continue
            _testAttachVDI(vdiClone, False, vmDom0[ht])
            if write and ht == thisHost:
                _testWrite(vdiClone, size, i)
            if i == iters - 1 and ht == thisHost:
                if size <= MAX_DISKTEST_SIZE or testMode != "basic":
                    _runDiskTest(vdiClone)
            _testDetachVDI(vdiClone)
    if not live:
        _testAttachVDI(vdi, False)
        if size <= MAX_DISKTEST_SIZE or testMode != "basic":
            _runDiskTest(vdi)

    _testDetachVDI(vdi)
    _testDestroyAll(sr)

def _testSnapshotSingle(sr, raw, live, write, size):
    text = "Single snapshot of "
    if raw:
        text += "raw VDI, "
    else:
        text += "VHD VDI, "
    if live:
        text += "live, "
    else:
        text += "offline, "
    if write:
        text += "non-"
    text += "empty, size %d" % (size)
    if not _nextTest(text): return

    vdi = _testCreateVDI(sr, size, raw)
    if live or write:
        _testAttachVDI(vdi, False)
        if write:
            _testWrite(vdi, size, 0)
        if not live:
            _testDetachVDI(vdi)

    vdiClone = _testSnapshotVDI(sr, vdi, live, True)
    try:
        _testAttachVDI(vdiClone, False)
        raise TestException("Attach of single snapshot %s succeeded" % vdiClone)
    except:
        pass
    try:
        vdiClone2 = _testSnapshotVDI(sr, vdiClone, live)
        raise TestException("Snapshot of single snapshot %s succeeded" % vdiClone)
    except:
        pass

    if not live:
        _testAttachVDI(vdi, False)
        if size <= MAX_DISKTEST_SIZE or testMode != "basic":
            _runDiskTest(vdi)

    _testDetachVDI(vdi)
    _testDestroyAll(sr)

def _testSnapshotInsufficientSpace(sr):
    text = "Snapshot: try with insufficient space"
    if not _nextTest(text): return

    info = sm.getInfoSR(sr)
    totalSize = int(info["physical-size"])
    size = totalSize * 998 / 1000
    vdi = _testCreateVDI(sr, size, True)
    try:
        vdiClone = sm.snapshotVDI(vdi)
        if _getThinProvisionFlag(info):
            _testAttachVDI(vdi, True)
            _testAttachVDI(vdiClone, True)
            raise TestException("Snapshot attached with insufficient space")
        raise TestException("Snapshot created with insufficient space")
    except (storagemanager.SMException, tutil.CommandException):
        pass
    _testDestroyAll(sr)

def _testSnapshotClobberBase(sr):
    text = "Snapshot: try to clobber base copy"
    if not _nextTest(text): return

    vdi = _testCreateVDI(sr, 10 * tutil.MiB, False)
    vdiClone = _testSnapshotVDI(sr, vdi, False)

    vg = sm.getPathSR(sr)
    cmd = "lvs %s" % vg
    text = tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_SUB)
    baseVDIFile = ""
    for line in text.split('\n'):
        if not line:
            continue
        uuid = line.split()[0][len("VHD-"):]
        if uuid != vdi and uuid != vdiClone:
            baseVDIFile = os.path.join(vg, "VHD-%s" % uuid)
    if not baseVDIFile:
        raise TestException("Base copy VDI LV not found")

    cmd = "lvchange -ay %s" % baseVDIFile
    tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_CMD)
    if not tutil.pathExists(baseVDIFile):
        raise TestException("Base copy VDI file not found (%s)" % baseVDIFile)
    try:
        f = open(baseVDIFile, "w")
        f.write("This is a readonly VDI clobber test")
        f.close()
        raise TestException("Base copy is writable")
    except IOError:
        pass
    cmd = "lvchange -an %s" % baseVDIFile
    tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_CMD)
    _testDestroyAll(sr)

def _testSnapshotFIST_do(sr, vdi, fistPoint, single, live, recovery):
    logger.log("FIST point %s" % fistPoint, 2)
    setFistPoint(fistPoint, True)
    if recovery:
        for rfist in SNAPSHOT_RECOVERY_FIST_POINTS:
            setFistPoint(rfist, True)
    try:
        vdiClone = _testSnapshotVDI(sr, vdi, live, single)
        raise TestException("%s did not cause failure" % fistPoint)
    except:
        if recovery:
            try:
                sm.refreshSR(sr)
                raise TestException("Recovery succeeded with rfist-0")
            except:
                pass
            setFistPoint(SNAPSHOT_RECOVERY_FIST_POINTS[0], False)
            try:
                sm.refreshSR(sr)
                raise TestException("Recovery succeeded with rfist-1")
            except:
                pass
            setFistPoint(SNAPSHOT_RECOVERY_FIST_POINTS[1], False)
        sm.refreshSR(sr)
    setFistPoint(fistPoint, False)

def _testSnapshotFIST(sr, single, raw, live, complexity, recovery):
    """Complexity param: 
        0 - single node; 
        1 - node snapshotted once, leaf empty;
        2 - node snapshotted once, leaf non-empty
    """
    text = "Snapshot: FIST points, "
    if single:
        text += "single-snap, "
    else:
        text += "double-snap, "
    if raw:
        text += "raw VDI, "
    else:
        text += "VHD VDI, "
    if live:
        text += "live, "
    else:
        text += "offline, "
    text += "complexity: %d" % complexity
    if recovery:
        text += " + recovery FIST"
    if not _nextTest(text): return

    setFistPoint("LVHDRT_exit", True)

    i = 1
    vdi = _testCreateVDI(sr, VDI_SIZE_DEFAULT, raw)
    _testAttachWrite(vdi, live, VDI_SIZE_DEFAULT, i, True)
    vdi2 = None
    if complexity > 0:
        vdi2 = _testSnapshotVDI(sr, vdi, False)
        if complexity == 2:
            i += 1
            _testAttachWrite(vdi, live, VDI_SIZE_DEFAULT, i, True)
            _testAttachWrite(vdi2, live, VDI_SIZE_DEFAULT, i, True)

    for fistPoint, succeeds in SNAPSHOT_FIST_POINTS.iteritems():
        _testSnapshotFIST_do(sr, vdi, fistPoint, single, live, recovery)

        # smoke tests
        vdi3 = _testSnapshotVDI(sr, vdi, live)
        _testAttachWrite(vdi3, False, VDI_SIZE_DEFAULT, i + 1)
        sm.destroyVDI(vdi3)
        if complexity > 0:
            vdi4 = _testSnapshotVDI(sr, vdi2, live)
            _testAttachWrite(vdi4, False, VDI_SIZE_DEFAULT, i + 1)
            sm.destroyVDI(vdi4)
        expectedNumVDIs = 2
        newClone = None
        if not single and succeeds:
            # find the new clone
            newClone = None
            leaves = sm.getLeafVDIs(sr)
            for leaf in leaves:
                if leaf != vdi and (complexity == 0 or leaf != vdi2):
                    newClone = leaf
                    sm.learnVDI(newClone)
                    break
            _testAttachWrite(newClone, False, VDI_SIZE_DEFAULT, i)
        i += 1
        _testAttachWrite(vdi, live, VDI_SIZE_DEFAULT, i)
        if complexity > 0:
            _testAttachWrite(vdi2, live, VDI_SIZE_DEFAULT, i)
            expectedNumVDIs += 3 # middle nodes won't be coalesceable
        if not single and succeeds:
            sm.destroyVDI(newClone)
        _waitForGC(sr, expectedNumVDIs)

    _testDestroyAll(sr)

def _testResizePrefill(sr, writeAmount):
    text = "Resize: prefilled with %d bytes" % writeAmount
    if not _nextTest(text): return

    vdi = _testCreateVDI(sr, 20 * tutil.MiB, False)
    _testAttachVDI(vdi, False)
    device = _getDevice(vdi)
    logger.log("Writing %d bytes to '%s'" % (writeAmount, device), 2)
    try:
        f = open(device, 'w')
        for i in range(0, writeAmount, 512):
            buf = str(i) * (512 / len(str(i)))
            padding = 512 - len(buf)
            buf += ">" * padding
            f.write(buf)
        f.close()
    except IOError:
        raise TestException("Failed to write into '%s'" % device)
    _testDetachVDI(vdi)

    sm.resizeVDI(vdi, 10 * tutil.GiB, False)
    _testAttachVDI(vdi, False)
    device = _getDevice(vdi)
    logger.log("Reading %d bytes from '%s'" % (writeAmount, device), 2)
    try:
        f = open(device, 'r')
        for i in range(0, writeAmount, 512):
            buf = f.read(512)
            expect = str(i) * (512 / len(str(i)))
            padding = 512 - len(expect)
            expect += ">" * padding
            if buf != expect:
                raise TestException("Contents mismatch: got: '%s', " % buf + \
                        "expected: '%s' (i=%d)" % (expect, i))
        f.close()
    except IOError:
        raise TestException("Failed to read '%s'" % device)

    _testDestroyAll(sr)

def _testResizeShrink(sr):
    text = "Resize: try to shrink"
    if not _nextTest(text): return

    vdi = _testCreateVDI(sr, 100 * tutil.MiB, False)
    try:
        sm.resizeVDI(vdi, 90 * tutil.MiB, False)
        raise TestException("Shrinking succeeded")
    except (storagemanager.SMException, tutil.CommandException):
        pass
    _testDestroyAll(sr)

def _testResizeInsufficientSpace(sr):
    text = "Resize: try with insufficient space"
    if not _nextTest(text): return

    info = sm.getInfoSR(sr)
    totalSize = int(info["physical-size"])
    vdi = _testCreateVDI(sr, 100 * tutil.MiB, False)
    try:
        sm.resizeVDI(vdi, totalSize, False)
        if _getThinProvisionFlag(info):
            _testAttachVDI(vdi, True)
            raise TestException("Resized to over SR capacity & attached")
        raise TestException("Resized to over SR capacity")
    except (storagemanager.SMException, tutil.CommandException):
        pass
    _testDestroyAll(sr)

def _testResizeVDI(sr, vdi, raw, live, size, snaps):
    if live:
        _testAttachVDI(vdi, False)
    sm.resizeVDI(vdi, size, live)
    if not raw:
        validateVHD(sr, vdi, live)
    info = sm.getInfoVDI(vdi)
    vsize = int(info["virtual-size"])
    if vsize < size:
        raise TestException("VDI size %d != %d (expected)" % (vsize, size))
    size = vsize # size may be rounded up

    # for the live case, in order to verify the size, we have to refresh the 
    # device by doing detach-reattach
    if live:
        _testDetachVDI(vdi)
    _testAttachVDI(vdi, False)
    if size <= MAX_DISKTEST_SIZE or testMode != "basic":
        _runDiskTest(vdi)

    if not live:
        _testDetachVDI(vdi)

    if snaps:
        vdiClone = _testSnapshotVDI(sr, vdi, live)
        nextSize = size + 50 * tutil.MiB
        _testResizeVDI(sr, vdiClone, False, live, nextSize, snaps - 1)

    if live:
        _testDetachVDI(vdi)

def _testResize(sr, raw, live, snaps):
    text = "Resize: "
    if live:
        text += "live, "
    else:
        text += "offline, "
    if raw:
        text += "Raw VDI"
    else:
        text += "VHD VDI"
    text += ", with %d snapshots" % snaps
    text += " (%d sizes)" % len(RESIZE_SIZES)
    if not _nextTest(text): return

    vdi = _testCreateVDI(sr, RESIZE_START_SIZE, raw)

    for size in RESIZE_SIZES:
        logger.log("(size %d)" % size, 1)
        _testResizeVDI(sr, vdi, raw, live, size, snaps)

    _testDestroyAll(sr)

def _testCoalesce(sr, size, raw, live, num, sizeDiff):
    """Test coalesce with the parameters:
        size: the size of the base VDI
        raw: whether the base VDI is raw or not
        live: whether the base VDI will be attached at the time of coalesce
        num: number of "coalesceable" VDIs
        sizeDiff: how much larger each child is compared to its parent"""
    text = "Coalesce: "
    if live:
        text += "live, "
    else:
        text += "offline, "
    if raw:
        text += "Raw VDI"
    else:
        text += "VHD VDI"
    text += ", size %d, %d coalesceable, size delta %d" % (size, num, sizeDiff)
    if not _nextTest(text): return

    vdiBase = _testCreateVDI(sr, size, raw)
    _testAttachWrite(vdiBase, live, size, 0, True)
    
    for i in range(num + 1):
        logger.log("Snapshot %d of %d" % ((i + 1), (num + 1)), 2)
        vdiClone = _testSnapshotVDI(sr, vdiBase, live)
        if sizeDiff:
            info = sm.getInfoVDI(vdiBase)
            size = int(info["virtual-size"]) + sizeDiff
            sm.resizeVDI(vdiBase, size, live)
        _testAttachWrite(vdiBase, live, size, (i + 1))
        sm.destroyVDI(vdiClone)
    
    _waitForGC(sr, 2)

    _testAttachWrite(vdiBase, live, size, (num + 1))
    _testDestroyAll(sr)

def _testCoalesceRelinkNonleaf(sr, size, live):
    """Test coalesce where the children to be relinked are not leaf nodes"""
    text = "Coalesce relink-nonleaf: "
    if live:
        text += "live, "
    else:
        text += "offline, "
    text += "size %d" % size
    if not _nextTest(text): return

    raw = False
    vdiBase = _testCreateVDI(sr, size, raw)
    _testAttachWrite(vdiBase, live, size, 0, True)
    
    num = 4
    toDestroy = None
    for i in range(2, num + 1):
        logger.log("Snapshot %d/%d" % (i, num), 2)
        vdiClone = _testSnapshotVDI(sr, vdiBase, live)
        if i == 2:
            toDestroy = vdiClone
        _testAttachWrite(vdiBase, live, size, (i - 1))
        if i == 3:
            _testAttachWrite(vdiClone, False, size, (i - 1))
            _testSnapshotVDI(sr, vdiClone, live)
    
    sm.destroyVDI(toDestroy)
    _waitForGC(sr, num * 2 - 1)

    _testAttachWrite(vdiBase, live, size, num)
    _testDestroyAll(sr)

def _testCoalesceNonRoot(sr, size, live):
    """Test coalesce where non-root nodes are involved"""
    text = "Coalesce non-root: "
    if live:
        text += "live, "
    else:
        text += "offline, "
    text += "size %d" % size
    if not _nextTest(text): return

    raw = False
    vdiBase = _testCreateVDI(sr, size, raw)
    _testAttachWrite(vdiBase, live, size, 0, True)
    
    num = 6
    toDestroy = None
    for i in range(1, num + 1):
        logger.log("Snapshot %d/%d" % (i, num), 2)
        vdiClone = _testSnapshotVDI(sr, vdiBase, live)
        if i == 1:
            toDestroy = vdiClone
        elif i % 2:
            sm.destroyVDI(vdiClone)
        _testAttachWrite(vdiBase, live, size, (i - 1))
    
    sm.destroyVDI(toDestroy)
    _waitForGC(sr, num + 1)

    _testAttachWrite(vdiBase, live, size, num)
    _testDestroyAll(sr)

def _testCoalesceRandomIO(sr, size, raw):
    """Write to disk randomly at a sector granularity, take a snapshot, write
    more (to the child now) at a sector granularity, take a hash, coalesce,
    compare the hash"""
    text = "Coalesce: random IO (size %s), " % size
    if raw:
        text += "raw VDI"
    else:
        text += "VHD VDI"
    if not _nextTest(text): return

    vdiBase = _testCreateVDI(sr, size, raw)
    _testAttachVDI(vdiBase, False)
    _testWriteRandom(vdiBase, size, COALESCE_RND_WFRAQ, 0)
    vdiClonePrev = _testSnapshotVDI(sr, vdiBase, True)
    for i in range(COALESCE_RND_ITERS):
        logger.log("Iteration %d" % (i + 1), 2)
        _testWriteRandom(vdiBase, size, COALESCE_RND_WFRAQ, i + 1)
        vdiClone = _testSnapshotVDI(sr, vdiBase, True)
        sm.destroyVDI(vdiClonePrev)
        vdiClonePrev = vdiClone
        validateVHD(sr, vdiBase, True)
        hashBefore = _getHash(vdiBase)
        _waitForGC(sr, 3, 2000)
        validateVHD(sr, vdiBase, True)
        hashAfter = _getHash(vdiBase)
        if hashBefore != hashAfter:
            raise TestException("Hash mismatch after coalesce")

    _testDestroyAll(sr)

def _testLeafCoalesce(sr, raw, live, sizeDiff, writeBlocks):
    """Test the basic functionality of coalesce-leaf"""
    text = "Leaf-clsce: "
    if live:
        text += "live, "
    else:
        text += "offline, "
    if raw:
        text += "Raw VDI"
    else:
        text += "VHD VDI"
    text += ", size delta %d, write %d blocks" % (sizeDiff, writeBlocks)
    if not _nextTest(text): return

    size = 300 * tutil.MiB
    vdi = _testCreateVDI(sr, size, raw)
    _testAttachWrite(vdi, live, size, 0, True)
    clone = _testSnapshotVDI(sr, vdi, live)
    if sizeDiff:
        info = sm.getInfoVDI(vdi)
        size = int(info["virtual-size"]) + sizeDiff
        sm.resizeVDI(vdi, size, live)
    if not live:
        _testAttachVDI(vdi, False)
    for i in range(writeBlocks):
        _testWrite(vdi, size, i + 1)
    if not live:
        _testDetachVDI(vdi)
    sm.destroyVDI(clone)
    
    _waitForGC(sr, 1)

    _testAttachWrite(vdi, live, size, writeBlocks + 1)
    _testDestroyAll(sr)

def _testLeafCoalesceConcurrency1(sr, fistPoint, raw, live, action, write):
    text = "Leaf-clsce: pt %s concurrent '%s'" % (fistPoint[-1], action)
    if live:
        text += ", live"
    else:
        text += ", offline"
    if raw:
        text += ", raw VDI"
    else:
        text += ", VHD VDI"
    if write:
        text += ", write"
    else:
        text += ", no write"
    if not _nextTest(text): return

    setFistPoint("LVHDRT_exit", False)

    size = 10 * tutil.GiB
    vdi = _testCreateVDI(sr, size, raw)
    _testAttachWrite(vdi, live, size, 0, True)
    clone = _testSnapshotVDI(sr, vdi, live)
    if not live:
        _testAttachVDI(vdi, False)
    for i in range(200):
        _testWrite(vdi, size, i + 1)
    if not live:
        _testDetachVDI(vdi)
    setFistPoint(fistPoint, True)
    sm.destroyVDI(clone)
    for i in range(NUM_RETRIES):
        logger.log("Waiting to hit the FIST point... (%d)" % (i), 2)
        flags = sm.getParam("sr", sr, "other-config")
        if flags.get(fistPoint) and flags[fistPoint] == "active":
            break
        time.sleep(RETRY_PERIOD)

    if write:
        _testAttachWrite(vdi, live, size, 1, False)

    if action == "destroy":
        sm.destroyVDI(vdi)
    else:
        clone = _testSnapshotVDI(sr, vdi, live)
        if write:
            _testAttachWrite(vdi, live, size, 1, False)

    time.sleep(20)
    for i in range(NUM_RETRIES):
        logger.log("Waiting to pass the FIST point... (%d)" % (i), 2)
        flags = sm.getParam("sr", sr, "other-config")
        if not flags.get(fistPoint):
            break
        time.sleep(RETRY_PERIOD)
    if action == "destroy":
        _waitForGC(sr, 0)
    else:
        time.sleep(6)
        setFistPoint(fistPoint, False)
        sm.destroyVDI(clone)
        _waitForGC(sr, 1)
    _testDestroyAll(sr)

def _testLeafCoalesceConcurrency2(sr, raw, live, sizeDiff, action):
    text = "Leaf-clsce: serializ'n with '%s'" % action
    if live:
        text += ", live"
    else:
        text += ", offline"
    if raw:
        text += ", Raw VDI"
    else:
        text += ", VHD VDI"
    text += ", size delta %d" % (sizeDiff)
    if not _nextTest(text): return

    size = 10 * tutil.GiB
    vdi = _testCreateVDI(sr, size, raw)
    _testAttachWrite(vdi, live, size, 0, True)
    clone = _testSnapshotVDI(sr, vdi, live)
    if sizeDiff:
        info = sm.getInfoVDI(vdi)
        size = int(info["virtual-size"]) + sizeDiff
        sm.resizeVDI(vdi, size, live)
    if not live:
        _testAttachVDI(vdi, False)
    for i in range(200):
        _testWrite(vdi, size, i + 1)
    if not live:
        _testDetachVDI(vdi)
    sm.setParam("vdi", vdi, "other-config:leaf-coalesce", "force")
    sm.destroyVDI(clone)
    time.sleep(3)
    i = 0
    while True:
        i += 1
        if action == "attach":
            _testAttachVDI(vdi, False)
            _testDetachVDI(vdi)
        elif action == "snap":
            clone = _testSnapshotVDI(sr, vdi, live)
            sm.destroyVDI(clone)
            break
        elif action == "resize":
            sm.resizeVDI(vdi, size + i * 10 * tutil.MiB, live)
        elif action == "destroy":
            sm.destroyVDI(vdi)
            _waitForGC(sr, 0)
        num = getNumFiles(sr)
        logger.log("Iteration %d: see %d files" % (i, num), 2)
        if num <= 1:
            break
    
    _waitForGC(sr, 1)
    _testDestroyAll(sr)

def _testLeafCoalesceInsufficientSpace(sr, raw):
    """Test coalesce-leaf with insufficient space"""
    text = "Leaf-clsce: insufficient space"
    if raw:
        text += ", Raw VDI"
    else:
        text += ", VHD VDI"
    if not _nextTest(text): return

    freeSpace = leaveFreeSpace(sr, 1 * tutil.GiB)

    size = freeSpace * 0.4
    if size % LVM_SIZE_INCREMENT:
        size = (int(size / LVM_SIZE_INCREMENT) + 1) * LVM_SIZE_INCREMENT
    vdi = _testCreateVDI(sr, size, raw)
    _testAttachVDI(vdi, False)
    for i in range(size / VHD_SIZE_INCREMENT):
        _testWrite(vdi, size, i)
    _testDetachVDI(vdi)

    vdiClone = _testSnapshotVDI(sr, vdi, False)
    _testAttachVDI(vdi, False)
    for i in range(size / VHD_SIZE_INCREMENT):
        _testWrite(vdi, size, i)
    _testDetachVDI(vdi)

    flags = sm.getParam("vdi", vdi, "other-config")
    if flags.get("leaf-coalesce"):
        assertRel("ne", flags["leaf-coalesce"], "offline")
    sm.destroyVDI(vdiClone)

    success = False
    for i in range(NUM_RETRIES):
        time.sleep(RETRY_PERIOD)
        flags = sm.getParam("vdi", vdi, "other-config")
        if flags.get("leaf-coalesce") and flags["leaf-coalesce"] == "offline":
            success = True
            break
        logger.log("Waiting for GC to mark VDI...")
    assertEqual(success, True)

    sm.setParam("vdi", vdi, "other-config:leaf-coalesce", "force")
    sm.refreshSR(sr)
    _waitForGC(sr, 2) # incl. the space-hogging VDI

    _testAttachWrite(vdi, False, size, (size / VHD_SIZE_INCREMENT + 1))
    _testDestroyAll(sr)

def _testLeafCoalesceFIST(sr, raw, live, recovery):
    """Test coalesce-leaf failure cases"""
    text = "Leaf-clsce: FIST"
    if raw:
        text += ", raw VDI"
    else:
        text += ", VHD VDI"
    if live:
        text += ", live"
    else:
        text += ", offline"
    if recovery:
        text += " + recovery FIST"
    if not _nextTest(text): return

    vdiBase = _testCreateVDI(sr, COALESCE_RND_SIZE, raw)
    _testAttachWrite(vdiBase, live, COALESCE_RND_SIZE, 0, True)

    setFistPoint("LVHDRT_exit", True)
    setFistPoint("LVHDRT_coaleaf_stop_after_recovery", True)

    i = 1
    for fistPoint, succeeds in COALEAF_FIST_POINTS.iteritems():
        logger.log("FIST point %s" % fistPoint, 2)
        setFistPoint(fistPoint, True)
#TODO: recovery fist
        vdiClone = _testSnapshotVDI(sr, vdiBase, live)
        sm.destroyVDI(vdiClone)
        time.sleep(6)
        sm.refreshSR(sr)
        # by now GC should have attempted leaf-coalesce, failed on the FIST 
        # point, recovered, and in the case of undo, stopped without 
        # re-attempting leaf-coalesce
        setFistPoint(fistPoint, False)
        if succeeds:
            _waitForGC(sr, 1)
        else:
            _waitForGC(sr, 2)
            _testAttachWrite(vdiBase, live, COALESCE_RND_SIZE, i)
            sm.setParam("vdi", vdiBase, "other-config:leaf-coalesce", "force")
            sm.refreshSR(sr)
            _waitForGC(sr, 1)
        assertEqual(getNumFiles(sr), 1)
        _testAttachWrite(vdiBase, live, COALESCE_RND_SIZE, i)
        i += 1

    _testDestroyAll(sr)

def _testJournalResize(sr, oldSize, newSize, fistPoint):
    """Test VHD-resize journal-based reverts"""
    text = "VHD-journal: %s resize %d -> %d" % (fistPoint, oldSize, newSize)
    if not _nextTest(text): return
    vdi = _testCreateVDI(sr, oldSize, False)
    _testAttachVDI(vdi, False)
    blocks = oldSize / VHD_SIZE_INCREMENT
    for i in range(blocks):
        _testWrite(vdi, oldSize, i)
    _testDetachVDI(vdi)

    sm.setParam("sr", sr, "other-config:testmode", fistPoint)
    try:
        sm.resizeVDI(vdi, newSize, False)
        raise TestException("Resize didn't fail as expected")
    except (storagemanager.SMException, tutil.CommandException):
        pass
    sm.setParam("sr", sr, "other-config:testmode", "")
    sm.refreshSR(sr)
    info = sm.getInfoVDI(vdi)
    actualSize = int(info["virtual-size"])
    if actualSize != oldSize:
        raise TestException("Size not restored: %d != %d" % \
                (oldSize, actualSize))
    _testAttachVDI(vdi, False)
    _testRead(vdi, oldSize, blocks - 1, True)
    _testDestroyAll(sr)

def _runTestAttachSR(sr):
    """Test SR.attach/detach"""
    logger.log("====> Test Group: SR ops", 0)
    text = "Detach/attach"
    if not _nextTest(text): return

    sm.unplugSR(sr)
    lockDir = "/var/run/locks/%s" % sr
    refcntDirLVM = "/var/run/refcount/lvm-%s" % sr
    if tutil.pathExists(lockDir):
        raise TestException("Lock dir not deleted on detach")
    if tutil.pathExists(refcntDirLVM):
        raise TestException("RefCount dir not deleted on detach")
    sm.plugSR(sr)

def _runTestProbeSR(sr):
    """Test SR.probe"""
    text = "Probe"
    if not _nextTest(text): return
    info = sm.getInfoPBD(sm.findThisPBD(sr))
    device_config = info["device-config"]
    dev = device_config.get("device")
    probeResult = sm.probe(sr, dev)
    if probeResult.find(sr) == -1:
        raise TestException("Probe did not find target SR on %s" % dev)

def _testReadCache(sr, chainSize, skipBlocks, writeAmount):
    """Test local read caching"""
    text = "Read cache for chain of %d, skipping %d, writing %d" % \
            (chainSize, skipBlocks, writeAmount)
    if not _nextTest(text): return
    size = 100 * tutil.MiB
    fillBlocks = 50
    vdi = _testCreateVDI(sr, size, False)
    sm.setParam("vdi", vdi, "allow-caching", "false")
    _testAttachVDI(vdi, False)
    _testWritePattern(vdi, fillBlocks, skipBlocks, writeAmount)
    _testDetachVDI(vdi)

    # create a chain of at least 2 because SM expects it
    vdiSnap = _testSnapshotVDI(sr, vdi, False)

    # read vdi partially to fill up the read cache partially 
    sm.setParam("vdi", vdi, "allow-caching", "true")
    _testAttachVDI(vdi, False)
    _testReadPattern(vdi, fillBlocks, False, skipBlocks, writeAmount)
    _testDetachVDI(vdi)

    # read vdi completely to fill up the read cache fully
    _testAttachVDI(vdi, False)
    _testReadPattern(vdi, fillBlocks, True, skipBlocks, writeAmount)
    _testDetachVDI(vdi)

    sm.setParam("vdi", vdi, "allow-caching", "false")

    # reparent the cache file so that we can see only what it has
    tmpVDI = _testCreateVDI(sr, size, False)
    tmpVHD = sm.getPathVDI(sr, tmpVDI)
    cacheSR = _findTargetSR("ext")
    cacheDir = sm.getPathSR(cacheSR)
    parentUUID = sm.getParentVDI(sr, vdi)
    readCachePath = "%s/%s.vhdcache" % (cacheDir, parentUUID)
    cmd = "vhd-util modify -n %s -p %s" % (readCachePath, tmpVHD)
    text = tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_CMD)

    # rename the cache so that we can plug it as a regular VDI
    newUuid = util.gen_uuid()
    newPath = "%s/%s.vhd" % (cacheDir, newUuid)
    logger.log("Renaming read cache %s => %s" % (readCachePath, newPath))
    os.rename(readCachePath, newPath)
    sm.refreshSR(cacheSR)
    sm.learnVDI(newUuid)
    time.sleep(3) # give time for the scan to complete

    _testAttachVDI(newUuid, False)
    _testReadPattern(newUuid, fillBlocks, True, skipBlocks, writeAmount, True)
    _testDetachVDI(newUuid)

    logger.log("Renaming back %s => %s" % (newPath, readCachePath))
    os.rename(newPath, readCachePath)
    sm.refreshSR(cacheSR)
    sm.unlearnVDI(newUuid)

    _testDestroyAll(sr)

def _testStandbyModeCacheBasic(sr):
    """Test Standby Mode write caching"""
    text = "Standby mode base case"
    if not _nextTest(text): return
    size = 100 * tutil.MiB
    vdi = _testCreateVDI(sr, size, False)
    sm.setParam("vdi", vdi, "allow-caching", "false")
    _testAttachVDI(vdi, False)
    for i in range(50):
        _testWrite(vdi, size, i, "first-pass")
    _testDetachVDI(vdi)

    # write to the scratch node
    sm.setParam("vdi", vdi, "allow-caching", "true")
    sm.setParam("vdi", vdi, "on-boot", "reset")
    _testAttachVDI(vdi, False)
    for i in range(50):
        _testWrite(vdi, size, i, "second-pass")
    _testDetachVDI(vdi)
    sm.setParam("vdi", vdi, "on-boot", "persist")
    sm.setParam("vdi", vdi, "allow-caching", "false")

    # Plug in the standby and read the block to match the "MARKER" pattern
    _testAttachVDI(vdi, False)
    _testRead(vdi, size, 49, True, "first-pass")
    _testDetachVDI(vdi)
    _testDestroyAll(sr)

def _testStandbyModeCacheOutOfSpace(sr):
    text = "Standby mode out of space"
    if not _nextTest(text): return

    size = 100 * tutil.MiB
    fillBlocks = 10
    skipBlocks = 0
    writeAmount = FULL_BLOCK_SIZE

    cacheSR = _findTargetSR("ext")
    freeSpace = leaveFreeSpace(cacheSR, 3 * tutil.MiB) # assuming 1GB SR!

    vdi = _testCreateVDI(sr, size, False)

    # write in standby mode
    sm.setParam("vdi", vdi, "allow-caching", "true")
    sm.setParam("vdi", vdi, "on-boot", "reset")
    _testAttachVDI(vdi, False)
    # should fail over during the write
    _testWritePattern(vdi, fillBlocks, skipBlocks, writeAmount)

    _testReadPattern(vdi, fillBlocks, True, skipBlocks, writeAmount)
    _testDetachVDI(vdi)

    sm.setParam("vdi", vdi, "allow-caching", "false")
    sm.setParam("vdi", vdi, "on-boot", "persist")

    _testAttachVDI(vdi, False)
    _testReadPattern(vdi, fillBlocks, True, skipBlocks, writeAmount, True)
    _testDetachVDI(vdi)

    _destroySpaceHogFile(cacheSR)
    _testDestroyAll(sr)

def _testMirrorModeCacheBasic(sr):
    """Test Mirror Mode write caching"""
    text = "Write cache base case"
    if not _nextTest(text): return
    size = 100 * tutil.MiB
    fillBlocks = 50
    skipBlocks = 1
    writeAmount = PART_BLOCK_SIZE
    vdi = _testCreateVDI(sr, size, False)
    sm.setParam("vdi", vdi, "allow-caching", "false")
    vdiSnap = _testSnapshotVDI(sr, vdi, False)

    # prefill the leaf without mirroring first
    _testAttachVDI(vdi, False)
    _testWritePattern(vdi, fillBlocks, skipBlocks, writeAmount, True)
    _testDetachVDI(vdi)

    # write in mirror mode
    sm.setParam("vdi", vdi, "allow-caching", "true")
    _testAttachVDI(vdi, False)
    _testWritePattern(vdi, fillBlocks, skipBlocks, writeAmount)
    _testDetachVDI(vdi)

    sm.setParam("vdi", vdi, "allow-caching", "false")

    # check the primary node
    _testAttachVDI(vdi, False)
    _testReadPattern(vdi, fillBlocks, True, skipBlocks, writeAmount, True)
    _testDetachVDI(vdi)

    # Now read from the local node only and to ensure that it has the same data
    tmpVDI = _testCreateVDI(sr, size, False)
    tmpVHD = sm.getPathVDI(sr, tmpVDI)
    cacheSR = _findTargetSR("ext")
    cacheDir = sm.getPathSR(cacheSR)
    writeCachePath = "%s/%s.vhdcache" % (cacheDir, vdi)
    cmd = "vhd-util modify -n %s -p %s" % (writeCachePath, tmpVHD)
    text = tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_CMD)

    # rename the cache so that we can plug it as a regular VDI
    newUuid = util.gen_uuid()
    newPath = "%s/%s.vhd" % (cacheDir, newUuid)
    logger.log("Renaming read cache %s => %s" % (writeCachePath, newPath))
    os.rename(writeCachePath, newPath)
    sm.refreshSR(cacheSR)
    sm.learnVDI(newUuid)
    time.sleep(3) # give time for the scan to complete

    _testAttachVDI(newUuid, False)
    _testReadPattern(newUuid, fillBlocks, True, skipBlocks, writeAmount, True)
    _testDetachVDI(newUuid)

    logger.log("Renaming back %s => %s" % (newPath, writeCachePath))
    os.rename(newPath, writeCachePath)
    sm.refreshSR(cacheSR)
    sm.unlearnVDI(newUuid)

    _testDestroyAll(sr)

def _testMirrorModeCacheOutOfSpace(sr):
    text = "Write cache out of space"
    if not _nextTest(text): return

    size = 100 * tutil.MiB
    fillBlocks = 3
    skipBlocks = 0
    writeAmount = FULL_BLOCK_SIZE

    vdi = _testCreateVDI(sr, size, False)
    sm.setParam("vdi", vdi, "allow-caching", "false")
    vdiSnap = _testSnapshotVDI(sr, vdi, False)

    cacheSR = _findTargetSR("ext")
    freeSpace = leaveFreeSpace(cacheSR, 3 * tutil.MiB) # assuming 1GB SR!

    # write in mirror mode
    sm.setParam("vdi", vdi, "allow-caching", "true")
    _testAttachVDI(vdi, False)
    # should fail over after writing first block
    _testWritePattern(vdi, fillBlocks, skipBlocks, writeAmount)
    _testReadPattern(vdi, fillBlocks, True, skipBlocks, writeAmount)
    _testDetachVDI(vdi)

    sm.setParam("vdi", vdi, "allow-caching", "false")

    # Now read from the local node only and to ensure that it has the first 
    # block
    tmpVDI = _testCreateVDI(sr, size, False)
    tmpVHD = sm.getPathVDI(sr, tmpVDI)
    cacheSR = _findTargetSR("ext")
    cacheDir = sm.getPathSR(cacheSR)
    writeCachePath = "%s/%s.vhdcache" % (cacheDir, vdi)
    cmd = "vhd-util modify -n %s -p %s" % (writeCachePath, tmpVHD)
    text = tutil.execCmd(cmd, 0, logger, storagemanager.LOG_LEVEL_CMD)

    # rename the cache so that we can plug it as a regular VDI
    newUuid = util.gen_uuid()
    newPath = "%s/%s.vhd" % (cacheDir, newUuid)
    logger.log("Renaming read cache %s => %s" % (writeCachePath, newPath))
    os.rename(writeCachePath, newPath)
    sm.refreshSR(cacheSR)
    sm.learnVDI(newUuid)
    time.sleep(3) # give time for the scan to complete

    _testAttachVDI(newUuid, False)
    _testReadPattern(newUuid, fillBlocks, True, skipBlocks, writeAmount, True)
    _testDetachVDI(newUuid)

    logger.log("Renaming back %s => %s" % (newPath, writeCachePath))
    os.rename(newPath, writeCachePath)
    sm.refreshSR(cacheSR)
    sm.unlearnVDI(newUuid)
    _destroySpaceHogFile(cacheSR)

    _testDestroyAll(sr)

WORKER_TYPE_ATTACH_DETACH = "worker1"
WORKER_TYPE_SNAPSHOT_DESTROY = "worker2"
WORKER_DIR = "/tmp"
WORKER_LOG_FILE = "log."
WORKER_ABORT_FILE = "abort-signal"
WORKER_FAIL_FILE = "fail."
WORKER_SUCCESS_FILE = "success."

def _checkAbort():
    files = os.listdir("/tmp")
    return WORKER_ABORT_FILE in files

def _doAttachDetachLoop(sr, vdi, numIters):
    for i in range(numIters):
        if _checkAbort():
            logger.log("Abort signaled - aborting")
            break
        try:
            logger.log("Iteration %d" % i)
            _testAttachVDI(vdi, False)
            _testDetachVDI(vdi)
        except Exception, e:
            logger.log("Failed: %s" % e)
            open("%s/%s%d" % (WORKER_DIR, WORKER_FAIL_FILE, os.getpid()), 'w').close()
            return
    logger.log("Completed successfully")
    open("%s/%s%d" % (WORKER_DIR, WORKER_SUCCESS_FILE, os.getpid()), 'w').close()

def _doSnapshotDestroyLoop(sr, vdi, numIters):
    for i in range(numIters):
        if _checkAbort():
            logger.log("Abort signaled - aborting")
            break
        try:
            logger.log("Iteration %d" % i)
            vdiSnap = _testSnapshotVDI(sr, vdi, False)
            sm.destroyVDI(vdiSnap)
        except Exception, e:
            logger.log("Failed: %s" % e)
            open("%s/%s%d" % (WORKER_DIR, WORKER_FAIL_FILE, os.getpid()), 'w').close()
            return
    logger.log("Completed successfully")
    open("%s/%s%d" % (WORKER_DIR, WORKER_SUCCESS_FILE, os.getpid()), 'w').close()

def _createWorker(sr, workerType, vdi, numIters, vbdLetter):
    global logger
    global sm
    pid = os.fork()
    if pid:
        logger.log("New %s for VDI %s created: PID %d" % (workerType, vdi, pid))
        return pid
    else:
        os.setpgrp()
        logger = tutil.Logger("%s/%s%d" % (WORKER_DIR, WORKER_LOG_FILE, os.getpid()), 3)
        sm.logger = logger
        sm.availableVBDLetters = [vbdLetter]
        if workerType == WORKER_TYPE_ATTACH_DETACH:
            _doAttachDetachLoop(sr, vdi, numIters)
        else:
            _doSnapshotDestroyLoop(sr, vdi, numIters)
        os._exit(0)

def _killAll(pids):
    for pid in pids:
        try:
            os.killpg(pid, signal.SIGKILL)
            logger.log("Killed PID %d" % pid)
        except Exception, e:
            logger.log("Error killing PID %d: %s" % (pid, e))

def _getStatus(pid):
    status = ""
    ext = ".%d" % pid
    files = filter(lambda x: x.endswith(ext), os.listdir("/tmp"))
    if len(files) == 0:
        return "none"
    successFile = "%s%d" % (WORKER_SUCCESS_FILE, pid)
    failFile = "%s%d" % (WORKER_FAIL_FILE, pid)
    if (not successFile in files) or (failFile in files):
        logger.log("%s, %s, %s" % (successFile, failFile, repr(files)))
        return "fail" # don't delete the temp files

    for name in files:
        os.unlink("%s/%s" % (WORKER_DIR, name))
    return "success"

def _testPause(sr, numVDIs, numIters):
    text = "Concurrent operations to test pause: %d VDIs, %d iterations" % (numVDIs, numIters)
    if not _nextTest(text): return

    size = 10 * tutil.MiB
    success = True
    pids = []
    vdis = []
    if WORKER_ABORT_FILE in os.listdir(WORKER_DIR):
        os.unlink("%s/%s" % (WORKER_DIR, WORKER_ABORT_FILE))

    for i in range(numVDIs):
        vdi = _testCreateVDI(sr, size, False)
        vdis.append(vdi)

    try:
        for i in range(numVDIs):
            pid = _createWorker(sr, WORKER_TYPE_ATTACH_DETACH, vdis[i], numIters, chr(ord('a') + i))
            pids.append(pid)
            pid = _createWorker(sr, WORKER_TYPE_SNAPSHOT_DESTROY, vdis[i], numIters, chr(ord('a') + i))
            pids.append(pid)
    except Exception, e:
        logger.log("Exception: %s, aborting workers" % e)
        open("%s/%s" % (WORKER_DIR, WORKER_ABORT_FILE), 'w').close()
        success = False
        #_killAll(pids)
        #raise

    # wait for all the worker processes to die
    while len(pids):
        pid, ret = os.wait()
        status = _getStatus(pid)
        logger.log("Worker %d done, status: %s" % (pid, status))
        pids.remove(pid)
        if status != "success":
            success = False
            open("%s/%s" % (WORKER_DIR, WORKER_ABORT_FILE), 'w').close()

    if not success:
        raise TestException("One or more workers did not succeed")

    _testDestroyAll(sr)


def runTestSR(sr):
    """Test SR ops"""
    _runTestAttachSR(sr)
    if vmDom0["slave"]:
        logger.log("<SR.probe test not implemented on pools>", 0)
        return
    if srType == "lvhd":
        _runTestProbeSR(sr)

def runCreateTests(sr):
    """ Tests VDI creation, attachment, and detachment on the specified SR. """

    logger.log("====> Test Group: Create", 0)
    _testDestroyAll(sr)
    
    rawValues = [False, True]
    if srType != "lvhd":
        rawValues = [False]
    vdis = []
    for raw in rawValues:
        for size in CREATE_SIZES:
            text = "Create "
            if raw:
                text += "Raw"
            else:
                text += "VHD"
            text += " VDI of size %d" % (size)
            if not _nextTest(text): continue
            vdi = _testCreateVDI(sr, size, raw)
            vdis.append(vdi)
            sm.refreshSR(sr)
            if size == CREATE_SIZES[1]:
                _testMultiAttach(vdi, size)

    hostTypes = ["master"]
    if vmDom0["slave"]:
        hostTypes.append("slave")
    for h in hostTypes:
        text = "Attach VDIs on %s" % h
        if not _nextTest(text): continue
        vbds = []
        for vdi in vdis:
            _testAttachVDI(vdi, False, vmDom0[h])
        logger.log("Detach VDI from %s" % h, 1)
        for vdi in vdis:
            _testDetachVDI(vdi)

    return _testDestroyAll(sr)

def runSnapshotTests(sr):
    logger.log("====> Test Group: Snapshot", 0)

    rawValues = [False, True]
    if srType != "lvhd":
        rawValues = [False]
    liveValues = [False, True]
    writeValues = [False, True]
    for raw in rawValues:
        for live in liveValues:
            for write in writeValues:
                for size in SNAPSHOT_SIZES:
                    _testSnapshot(sr, raw, live, write, size, \
                            SNAPSHOT_NUM_ITERS)

    if srType == "lvhd":
        for raw in rawValues:
            for live in liveValues:
                for write in writeValues:
                    for size in SNAPSHOT_SIZES:
                        _testSnapshotSingle(sr, raw, live, write, size)

        _testSnapshotInsufficientSpace(sr)
        _testSnapshotClobberBase(sr)
        try:
            for single in [False, True]:
                for raw in [False, True]:
                    for live in [False]:
                        for complexity in [0, 1, 2]:
                            for recovery in [False, True]:
                                _testSnapshotFIST(sr, single, raw, live, \
                                        complexity, recovery)
        finally:
            for fistPoint in SNAPSHOT_FIST_POINTS.keys():
                setFistPoint(fistPoint, False)
            for fistPoint in SNAPSHOT_RECOVERY_FIST_POINTS:
                setFistPoint(fistPoint, False)

def runResizeTests(sr):
    logger.log("====> Test Group: Resize", 0)
    rawValues = [False, True]
    if srType != "lvhd":
        rawValues = [False]
    liveValues = [False]
    for raw in rawValues:
        for live in liveValues:
            _testResize(sr, raw, live, RESIZE_SNAPSHOTS)

    for size in RESIZE_PREFILL_SIZES:
        _testResizePrefill(sr, size)

    _testResizeShrink(sr)
    if srType == "lvhd":
        _testResizeInsufficientSpace(sr)

def runPauseTests(sr):
    logger.log("====> Test Group: Pause", 0)
    numVDIs = 10
    numIters = 100
    _testPause(sr, numVDIs, numIters)

def runCoalesceTests(sr):
    logger.log("====> Test Group: Coalesce", 0)

    rawVals = [False, True]
    resizeVals = [0, 100 * tutil.MiB]
    if srType != "lvhd":
        rawVals = [False]
    size = 30 * tutil.MiB
    for raw in rawVals:
        for live in [False, True]:
            for num in [1, 10]:
                for diff in resizeVals:
                    if live and diff:
                        continue
                    _testCoalesce(sr, size, raw, live, num, diff)

    for live in [False, True]:
        _testCoalesceRelinkNonleaf(sr, size, live)
    for live in [False, True]:
        _testCoalesceNonRoot(sr, size, live)
    for live in [False, True]:
        _testCoalesceRandomIO(sr, COALESCE_RND_SIZE, live)

def runLeafTests(sr):
    logger.log("====> Test Group: Leaf coalesce", 0)

    rawVals = [False, True]
    if srType != "lvhd":
        rawVals = [False]
    liveVals = [False, True]
    resizeVals = [0, 100 * tutil.MiB]
    writeBlocks = [1, 100]
    for raw in rawVals:
        for live in liveVals:
            for diff in resizeVals:
                for write in writeBlocks:
                    if diff and live:
                        continue # not supported
                    _testLeafCoalesce(sr, raw, live, diff, write)

    for fistPoint in COALEAF_CONCURRENCY_FIST_POINTS:
        for raw in rawVals:
            for live in liveVals:
                for action in ["snap", "destroy"]:
                    for write in [False, True]:
                        if live and action == "destroy":
                            continue
                        try:
                            _testLeafCoalesceConcurrency1(sr, fistPoint, raw, \
                                    live, action, write)
                        finally:
                            for fp in COALEAF_CONCURRENCY_FIST_POINTS:
                                setFistPoint(fp, False)

    for raw in rawVals:
        for live in liveVals:
            for diff in resizeVals:
                for action in ["attach", "snap", "resize", "destroy"]:
                    if live and (diff or action != "snap"):
                        continue
                    _testLeafCoalesceConcurrency2(sr, raw, live, diff, action)

    for raw in rawVals:
        _testLeafCoalesceInsufficientSpace(sr, raw)

    try:
        for raw in rawVals:
            for live in liveVals:
                for recovery in [False]: #, True]:
                    _testLeafCoalesceFIST(sr, raw, live, recovery)
    finally:
        for fistPoint in COALEAF_FIST_POINTS:
            setFistPoint(fistPoint, False)
        setFistPoint("LVHDRT_coaleaf_stop_after_recovery", False)

def runReadCacheTests(sr):
    logger.log("====> Test Group: Read Caching", 0)
    for  chainSize in [1]: #, 2]:
        for skipBlocks in [0, 2]:
            for writeAmount in [FULL_BLOCK_SIZE, PART_BLOCK_SIZE]:
                _testReadCache(sr, chainSize, skipBlocks, writeAmount)

def runMirrorCacheTests(sr):
    logger.log("====> Test Group: Mirror Mode Write Caching", 0)
    _testMirrorModeCacheBasic(sr)
    _testMirrorModeCacheOutOfSpace(sr)

def runStandbyCacheTests(sr):
    logger.log("====> Test Group: Standby Mode Write Caching", 0)
    _testStandbyModeCacheBasic(sr)
    _testStandbyModeCacheOutOfSpace(sr)

def runCacheTests(sr):
    runReadCacheTests(sr)
    runMirrorCacheTests(sr)
    runStandbyCacheTests(sr)

def runJournalTests(sr):
    logger.log("====> Test Group: VHD-journaling", 0)

    initSize = 100 * tutil.MiB
    for fistPoint in RESIZE_FIST_POINTS:
        for newSize in [1 * tutil.GiB, 10 * tutil.GiB]:
            _testJournalResize(sr, initSize, newSize, fistPoint)

def usage():
    print "Params: -t lvhd|lvm|ext|nfs -m basic|extended|coalesce|jvhd " \
        "[-v 1..4 log verbosity (default 3)] " \
        "[-a NUM skip to test NUM] [-b NUM stop after test NUM] " \
        "[-h print this help]"
    print
    print "The modes are: \n" \
        " - basic:    tests SR attach,detach,probe; " \
        "VDI create,snapshot,resize (LVHD only)\n"\
        " - pause:    tests concurrent attach/detach and snapshot/delete\n" \
        " - extended: same as basic, but don't skip long disk checks and " \
        "add more parameter values (LVHD only)\n" \
        " - coalesce: test coalescing\n" \
        " - caching: test local caching\n" \
        " - jvhd:     test VHD journaling (LVHD only)\n"
    print
    print "The script expects an empty SR of the specified type to be " \
        "available (if there are multiple SRs of the specified type, the " \
        "default SR is used if it is the right type). " \
        "No cleanup is performed upon a failure to assist debugging."
    sys.exit(0)

def main():
    global logger
    global vmDom0
    global thisHost
    global testMode
    global skipTo, stopAfter
    global srType
    global sm

    # Controls whether the SR itself must be tested.
    testSR = False

    # Controls whether the SR creation test must be performed.
    testCreate = False
    testSnapshot = False
    testResize = False
    testPause = False
    testCoalesce = False
    testCaching = False
    testJVHD = False
    verbosity = 3
    mode = ""

    try:
        opts, args = getopt.getopt(sys.argv[1:], "v:t:m:a:b:h", "")
    except getopt.GetoptError:
        usage()

    for o, a in opts:
        if o in ["-t"]:
            srType = a
        if o in ["-m"]:
            testMode = a
        if o in ["-v"]:
            verbosity = int(a)
        if o in ["-a"]:
            skipTo = int(a)
        if o in ["-b"]:
            stopAfter = int(a)
        if o in ["-h"]:
            usage()

    if not srType.replace("oiscsi", "") in ["lvhd", "ext", "lvm", "nfs"]:
        usage()
    if not testMode in ["basic", "pause", "extended", "coalesce", "caching", "jvhd"]:
        usage()

    logger = tutil.Logger(LOG_FILE, verbosity)
    print "Log file in %s" % LOG_FILE
    logger.log("Testing: %s" % testMode, 0)
    if skipTo:
        logger.log("Skipping to test %d" % skipTo, 0)
    if stopAfter < 100000:
        logger.log("Will stop after test %d" % stopAfter, 0)

    sm = storagemanager.StorageManager.getInstance(logger)
    masterVM, slaveVM = sm.getVMs()
    if slaveVM:
        if sm.getThisDom0() == masterVM:
            logger.log("Running on the master of a pool", 0)
        else:
            thisHost = "slave"
            logger.log("Running on a slave in a pool", 0)
    srTypeActual = srType
    if srType.startswith("lvm") or srType.startswith("lvhd"):
        srType = "lvhd"

    vmDom0["master"] = masterVM
    vmDom0["slave"] = slaveVM

    if testMode in ["basic", "extended"]:
        testSR = True
        testCreate = True
        testSnapshot = True
        testResize = True
    elif testMode in ["pause"]:
        testPause = True
    elif testMode in ["coalesce"]:
        testCoalesce = True
    elif testMode in ["caching"]:
        testCaching = True
    elif testMode in ["jvhd"]:
        if srType != "lvhd":
            raise TestException("VHD journaling tests only available for LVHD")
        testJVHD = True

    success = True
    startTime = time.time()
    try:
        # Get any suitable SR of the specified type.
        sr = _findTargetSR(srTypeActual)
        if not sr:
            return 1
        if testSR:
            runTestSR(sr)

        # TODO What does this do?
        setFistPoint("testsm_clone_allow_raw", True)

        if testCreate:
            runCreateTests(sr)
        if testSnapshot:
            runSnapshotTests(sr)
        if testResize:
            runResizeTests(sr)
        if testPause:
            runPauseTests(sr)
        if testCoalesce:
            runLeafTests(sr)
            runCoalesceTests(sr)
        if testCaching:
            if srType != "nfs":
                raise TestException("Caching tests only available for NFS SR")
            runCacheTests(sr)
        if testJVHD:
            runJournalTests(sr)
    except (tutil.CommandException, storagemanager.SMException,
            util.SMException, TestException):
        info = sys.exc_info()
        tb = reduce(lambda a, b: "%s%s" % (a, b), traceback.format_tb(info[2]))
        logger.log("Error: %s" % info[0], 0)
        logger.log("Description: %s" % info[1], 0)
        logger.log("Stack trace:\n%s" % tb, 0)
        success = False
    except StopRequest, e:
        logger.log("Stop requested: %s" % e, 0)
        pass

    setFistPoint("testsm_clone_allow_raw", False)

    endTime = time.time()
    totalTime = int(endTime - startTime)
    strTotalTime = ""
    if totalTime > 3600:
        strTotalTime += "%d:" % (totalTime / 3600)
        totalTime = totalTime % 3600
    strTotalTime += "%02d:" % (totalTime / 60)
    totalTime = totalTime % 60
    strTotalTime += "%02d" % totalTime
    logger.log("Total time: %s" % strTotalTime, 0)
    logger.log("", 0)

    if success:
        logger.log("PASS", 0)
        return 0

    logger.log("FAIL", 0)
    return 1

main()
