#!/usr/bin/python
import unittest
import logger
import tutil
import storagemanager
import sys
import os

sys.path.append('/opt/xensource/sm')
import srmetadata

# UUID of the SR used for the test.
sr_uuid = None

# UUID of the VM used for the test (for attaching VDIs etc.)
vm_uuid = None

class TestContentIDEquality(unittest.TestCase):
    """Creates a VDI, snapshots it, and verifies that that all three VDIS
    (original, snapshot, base) have the same content ID."""

    def setUp(self):
        self.vdi_uuid = None
        self.snap_uuid = None

    def test(self):
        self.vdi_uuid = sm._createVDI(sr_uuid, size = 16 * (2**20))
        self.snap_uuid = sm._snapshotVDI(self.vdi_uuid)
        base_uuid = sm._vdi_get_parent(self.vdi_uuid)

        vdi_contid = srmetadata.get_content_id(sr_uuid, self.vdi_uuid)
        snap_contid = srmetadata.get_content_id(sr_uuid, self.snap_uuid)
        base_contid = srmetadata.get_content_id(sr_uuid, base_uuid)
        
        self.assertEqual(vdi_contid, snap_contid)
        self.assertEqual(vdi_contid, base_contid)

    def tearDown(self):
        if self.vdi_uuid:
            sm._destroyVDI(self.vdi_uuid)
        if self.snap_uuid:
            sm._destroyVDI(self.snap_uuid)

class TestContentIDErasedOnAttch(unittest.TestCase):

    def setUp(self):
        self.vdi_uuid = None
        self.vbd_uuid = None

    def test(self):
        self.vdi_uuid = sm._createVDI(sr_uuid)
        (self.vbd_uuid, device) = sm._vdi_attach(self.vdi_uuid, vm_uuid)
        self.assertEqual('', srmetadata.get_content_id(sr_uuid, self.vdi_uuid))

    def tearDown(self):
        if self.vbd_uuid:
            sm._unplugVBD(self.vbd_uuid)
        if self.vdi_uuid:
            sm._destroyVDI(self.vdi_uuid)

if __name__ == '__main__':

    logger.logger = tutil.Logger('/tmp/test_content_id.log', 2)
    sm = storagemanager.StorageManager.getInstance(logger.logger)

    # FIXME check args
    if len(sys.argv) < 3:
        print 'usage: ' + sys.argv[0] + ' <sr-uuid> <vm-uuid>'
        sys.exit(os.EX_USAGE)

    sr_uuid = sys.argv[1]
    vm_uuid = sys.argv[2]

    # Remove sr_uuid and vm_uuid args before passing control to unittest.
    del sys.argv[1:3]

    unittest.main()
