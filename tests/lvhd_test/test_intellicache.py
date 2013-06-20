#!/usr/bin/python
#
#   Basic IntelliCache tests. Assumes the environment has been setup
#   accordingly: a VM has been installed on an NFS SR and caching has been
#   enabled on the host. libaio must be installed on the VM.
#   TODO review whether the aforementioned requirements are well-explained
#
#   It shuts down the VM, creates a VDI, enables caching on it, starts the VM,
#   and performs the following operations:
#   1.  it attaches the VDI, with caching enabled (in persistent or reset mode)
#       or disabled, verifies previously written data.
#   2.  writes some data, verifies
#   3.  detaches the VDI, jumps to step 1.
#
#   The VM must be a Linux machine, and the root user/pass must be
#   root/xenroot.
#
#   TODO Once this script is tested, it should become a unit test.

import random
import storagemanager
import string
import sys
import threading
import time
import tutil
import unittest
import logger
import os

def main(argv):

    vm_uuid = None # UUID of the VM inside which biotest will run
    sr_uuid = None # UUID of the SR where the VDI will be created

    # total time, in seconds, for the test (includes time needed for the VM    
    # to reboot)
    ttl = 180

    ttr = 60 # timeout for the VM to restart
    size_gb = 1 # VDI size in GiB
    mbytes = 32 # number of MiB to be written by biotest
    caching_prob = .66
    persistence_prob = .66

    # Controls how much time will be spent performing plug-biotest-unplug
    # operations (after the cache/persistence mode has been selected).
    worktime = float(ttl) / 6

     # Seed to use for the random number generator. Use the same seed across
     # runs to ensure consistent behavior.
    seed = None

    assert mbytes <= size_gb * (2**10)

    verbose = False

    # Parse the arguments.
    for arg in argv[1:]:
        fields = arg.split('=')
        if 'vm-uuid' == fields[0]:
            vm_uuid = fields[1]
        elif 'sr-uuid' == fields[0]:
            sr_uuid = fields[1]
        elif 'ttl' == fields[0]:
            ttl = int(fields[1])
        elif 'ttr'== fields[0]:
            ttr = int(fields[1])
        elif 'size' == fields[0]:
            size_gb =int(fields[1])
        elif 'caching-prob' == fields[0]:
            caching_prob = float(fields[1])
            assert caching_prob >= 0 and caching_prob <= 1 # FIXME
        elif 'persistence-prob' == fields[0]:
            persistence_prob = float(fields[1])
            assert persistence_prob >= 0 and persistence_prob <= 1 # FIXME
        elif 'seed' == fields[0]:
            seed = fields[1]
            random.seed(seed)
        elif 'worktime' == fields[0]:
            worktime = int(fields[1])
        elif '-v' == fields[0] or '--verbose' == fields[0]:
            verbose = True
        else:
            print 'invalid key \'' + fields[0] + '\''
            return 2

    logger.logger = tutil.Logger('/tmp/test_intellicache.log', 2)
    sm = storagemanager.StorageManager.getInstance(logger.logger)

    if None == vm_uuid:
        # If no VM has been specified, pick any one.
        # TODO not implemented
        print 'no VM specified'
        return os.EX_CONFIG

    if None == sr_uuid:
        # If no SR has been specified, pick one that has caching enabled.
        print 'no SR specified'
        # TODO not implemented
        return os.EX_CONFIG

    # Ensure the SR has caching enabled.
    sr_params = sm._getInfoSR(sr_uuid)
    if 'nfs'!= sr_params['type'] or 'true' != sr_params['shared']:
        return os.EX_CONFIG

    # FIXME is the following necessary?
    #assert 'true' == sr_params['local-cache-enabled']

    # Ensure caching has been enabled on the host and that there is a SR that
    # acts as a cache.
    local_cache_sr = sm._host_get_local_cache_sr()
    if not tutil.validateUUID(local_cache_sr):
        print 'caching not enabled'
        sys.exit(os.EX_CONFIG)

    # FIXME other power states not taken into account
    if not sm._vm_is_running(vm_uuid):
        sm._vm_start(vm_uuid)
        time.sleep(ttr)

    # Put the binaries in the VM.
    vm_ip = tutil.vm_get_ip(vm_uuid)
    assert None != vm_ip
    tutil.scp(vm_ip, '../biotest', '/var/tmp/biotest')

    # Create a VDI on the specified SR.
    vdi_uuid = sm._createVDI(sr_uuid, size_gb * (2**30))

    if verbose:
        print 'test VDI is ' + vdi_uuid

    # The original VHD file that backs the VDI on the NFS SR.
    vdi_file = '/var/run/sr-mount/' + sr_uuid + '/' + vdi_uuid + '.vhd'

    # The VHD file on the local SR that backs the remote VDI.
    cache_file = '/var/run/sr-mount/' + local_cache_sr + '/' + vdi_uuid + \
            '.vhdcache'

    # Snapshot the VDI so it can be cached.
    vdi_snapshot_uuid = sm._snapshotVDI(vdi_uuid)

    # Create a VDB for it.
    vbd_uuid = sm._createVBD(vdi_uuid, vm_uuid)

    cache_on = False
    first_iter = True
    endt = time.time() + ttl

    # stats
    stats_cached = 0
    stats_total = 0
    stats_plug_unplug_loops = 0
    stats_persistent = 0

    while time.time() < endt:
        # In the beginning of each iteration the VDI is expected to be
        # detached.
        #
        # Enable/disable caching on the VDI.
        vm_shutdown = False
        if caching_prob >= random.random():

            # Select persistence mode.
            persistent = (persistence_prob >= random.random())

            if not cache_on: # enable only if not already enabled
                sm._vm_shutdown(vm_uuid)
                sm._vdi_enable_caching(vdi_uuid, persistent)
                cache_on = True
                vm_shutdown = True
            elif sm._vdi_cache_persistence(vdi_uuid) != persistent:
                sm._vm_shutdown(vm_uuid)
                sm._vdi_cache_persistence(vdi_uuid, persistent)
                vm_shutdown = True

        else: # disable caching
            if cache_on:
                # If not already disabled, we need to shut down the VM first.
                sm._vm_shutdown(vm_uuid)
                sm._vdi_disable_caching(vdi_uuid)
                cache_on = False
                vm_shutdown = True

            # Ensure that the cache file is gone.
            assert not os.path.exists(cache_file)

        if verbose:
            print 'cache ' + str(cache_on),
            if cache_on:
                print ', persistent ' + str(persistent)
            else:
                print

        # If the VM was restarted, it's IP address may have changed.
        if True == vm_shutdown:

            sm._vm_start(vm_uuid)

            # We must wait for the VM to boot.
            time.sleep(ttr)

            sm._unplugVBD(vbd_uuid)
            vm_ip = tutil.vm_get_ip(vm_uuid)

        # Check existing data.
        if first_iter:
            first_iter = False
        else:
            sm._plugVBD(vbd_uuid)
            dev = '/dev/' + sm._vbd_get_bdev(vbd_uuid)
            tutil.ssh(vm_ip, '/var/tmp/biotest -t ' + dev + ' -m ' + \
                    str(mbytes) + ' -v')
            sm._unplugVBD(vbd_uuid)

        endt2 = time.time() + worktime
        stats_prev_plug_unplug_loops = stats_plug_unplug_loops
        while time.time() < endt2:

            # Store the old size of the cache file so we can ensure data are
            # actually written to the cache (as a sanity check). The first
            # loop must be skipped because no data have been written yet.
            if cache_on:
                if stats_prev_plug_unplug_loops > stats_plug_unplug_loops:
                    cache_file_prev_size = os.path.getsize(cache_file)
                else:
                    cache_file_prev_size = 0

            # FIXME check that VDI grows, or, if in non-persistent mode, that
            # it doesn't grow 
            vdi_file_prev_size = os.path.getsize(vdi_file)

            sm._plugVBD(vbd_uuid)
            dev = '/dev/' + sm._vbd_get_bdev(vbd_uuid)
            # XXX timeout based on volume size. Preliminary tests show that the
            # average speed for volumes larger than 64 MB is 2 MB/s, let's
            # assume .5 MB/s.
            tutil.ssh(vm_ip, '/var/tmp/biotest -t ' + dev + ' -m ' + \
                    str(mbytes), timeout = 2 * mbytes)

            stats_plug_unplug_loops += 1
            
            if cache_on:
                # Ensure that the cache file has grown.
                assert os.path.getsize(cache_file) >= cache_file_prev_size

                new_size = os.path.getsize(vdi_file)
                if persistent:
                    # Persistent cache mode: ensure the original VDI has grown.
                    if new_size < vdi_file_prev_size:
                        print 'new VDI file size (' + str(new_size) \
                                + ') should be bigger than old one (' \
                                + str(vdi_file_prev_size) + ')'
                        assert False
                else:
                    # Reset cache mode: no writes should reach the VDI on the
                    # NFS SR
                    if not new_size == vdi_file_prev_size:
                        print 'VDI on the shared SR has been modified whilst in reset mode, old size ' + str(vdi_file_prev_size) + ', new size ' + str(new_size)

            sm._unplugVBD(vbd_uuid)

        # Update stats.
        assert stats_plug_unplug_loops >= stats_prev_plug_unplug_loops 
        if stats_plug_unplug_loops > stats_prev_plug_unplug_loops:
            stats_total += 1
            if cache_on:
                stats_cached += 1
                if persistent:
                    stats_persistent += 1

    sm._destroyVBD(vbd_uuid)
    sm._destroyVDI(vdi_snapshot_uuid)
    sm._destroyVDI(vdi_uuid)

    print 'total ' + str(stats_total) + ', cached ' + str(stats_cached) \
            + ', plug/unplug loops ' + str(stats_plug_unplug_loops) + \
            ', persistent ' + str(stats_persistent)
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
