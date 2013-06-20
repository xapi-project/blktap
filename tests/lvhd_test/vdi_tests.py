#!/usr/bin/python

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

sys.path.append('/opt/xensource/sm')

# UUID of the SR used for the test.
sr_uuid = None

# UUID of the VM used for the test (for attaching VDIs etc.)
vm_uuid = None

# TODO define a naming convention for unit tests
class VDIBasicTest(unittest.TestCase):

    def setUp(self):
        """Create a few VDIs on the default SR."""
        self.sm = storagemanager.StorageManager.getInstance(logger.logger)

        # Create some VDIs.
        # TODO use variables that control percentage of usable SR size, average
        # min, and max VDI size, min and max number of VDIs, etc, raw vs. vhd
        # probability etc.
        logger.logger.log('creating VDIs')
        self.vdis = []
        for i in range(10):
            vdi = self.sm._createVDI(sr_uuid, size = 2**24)
            self.vdis.append(vdi)
            logger.logger.log('created VDI ' + vdi)

    def vdi_cntrl(self, vdi, ttl):
        """Performs attach/detach operation on the specified VDI, until it
        reaches the ttl time limit (expressed in seconds)."""
        
        endt = time.time() + ttl
        attached = False
        plugs = 0
        unplugs = 0

        while time.time() < endt:
            if attached:
                self.sm.unplugVDI(vdi)
                attached = False
                unplugs += 1
            else:
                self.sm.plugVDI(vdi, 'ro')
                attached = True
                plugs += 1
            tts = random.randint(1, 1000) / float(1000)
            logger.logger.log('sleeping for ' + str(tts) + ' ms')
            time.sleep(tts)

        if attached:
            self.sm.unplugVDI(vdi)

        logger.logger.log('plugs=' + str(plugs) + ', unplugs=' + str(unplugs))

    def test_attach_detach_concur(self):
        # TODO To which VM are the VDIs attached?
        # For each VDI create a thread. The thread will perform a attach/detach
        # operation and will sleep for a random interval.
        threads = []
        for vdi in self.vdis:
            threads.append(
                    threading.Thread(
                        target = self.vdi_cntrl,
                        args = (vdi, 30)))

        # Start the threads
        logger.logger.log('starting ' + str(len(threads)) + ' threads')
        for thread in threads:
            thread.start()

        # Wait for the threads to finish.
        logger.logger.log('waiting for threads to finish')
        for thread in threads:
            thread.join()

        logger.logger.log('threads finished')

    def test_attach_simultaneously(self):
        """Simoultaneously attaches some VDIs to a VM. All the VDIs belong
        to the same SR."""

        # Create the VBDs and threads beforehand.
        threads = []
        vbds = []
        for vdi in self.vdis:
            vbd = self.sm._createVBD(vdi, vm_uuid)
            vbds.append(vbd)

        # Repeat the test a few times.
        for i in range(16):
            threads = []
            threads.append(
                threading.Thread(target = sm._plugVBD, args = [vbd]))

            for thread in threads:
                thread.start()
            
            for thread in threads:
                thread.join()

            for vbd in vbds:
                sm._unplugVBD(vbd)

        for vbd in vbds:
            sm._destroyVBD(vbd)            

    def tearDown(self):
        for vdi in self.vdis:
            self.sm._destroyVDI(vdi)

if __name__ == '__main__':

    logger.logger = tutil.Logger('/tmp/test_vdi_attach_detach.log', 3)
    sm = storagemanager.StorageManager.getInstance(logger.logger)

    # FIXME check args
    if len(sys.argv) < 3:
        print 'usage: ' + sys.argv[0] + ' <sr-uuid> <vm-uuid>'
        sys.exit(os.EX_USAGE)

    sr_uuid = sys.argv[1]
    vm_uuid = sys.argv[2]

    # Remove the vm_uuid arg before passing control to unittest.
    del sys.argv[1:3]

    unittest.main()
