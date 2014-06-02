import unittest
import os

import testlib


class TestTestContext(unittest.TestCase):
    def test_generate_inventory_file(self):
        context = testlib.TestContext()
        context.inventory = dict(key='value')

        self.assertEquals("key='value'", context.generate_inventory_contents())

    @testlib.with_context
    def test_adapter_adds_scsi_host_entry(self, context):
        context.adapter()

        self.assertEquals(['host0'], os.listdir('/sys/class/scsi_host'))

    @testlib.with_context
    def test_add_disk_adds_scsi_disk_entry(self, context):
        import glob
        adapter = context.adapter()
        adapter.add_disk()

        self.assertEquals(
            ['/sys/class/scsi_disk/0:0:0:0'],
            glob.glob('/sys/class/scsi_disk/0*'))

    @testlib.with_context
    def test_add_disk_adds_scsibus_entry(self, context):
        import glob
        adapter = context.adapter()
        adapter.long_id = 'HELLO'
        adapter.add_disk()

        self.assertEquals(
            ['/dev/disk/by-scsibus/HELLO-0:0:0:0'],
            glob.glob('/dev/disk/by-scsibus/*'))

    @testlib.with_context
    def test_add_disk_adds_device(self, context):
        adapter = context.adapter()
        adapter.add_disk()

        self.assertEquals(
            ['sda'],
            os.listdir('/sys/class/scsi_disk/0:0:0:0/device/block'))

    @testlib.with_context
    def test_add_disk_adds_disk_by_id_entry(self, context):
        adapter = context.adapter()
        disk = adapter.add_disk()
        disk.long_id = 'SOMEID'

        self.assertEquals(['SOMEID'], os.listdir('/dev/disk/by-id'))

    @testlib.with_context
    def test_add_disk_adds_glob(self, context):
        import glob
        adapter = context.adapter()
        disk = adapter.add_disk()

        self.assertEquals(['/dev/disk/by-id'], glob.glob('/dev/disk/by-id'))

    @testlib.with_context
    def test_add_disk_path_exists(self, context):
        adapter = context.adapter()
        disk = adapter.add_disk()

        self.assertTrue(os.path.exists('/dev/disk/by-id'))

    @testlib.with_context
    def test_add_parameter_parameter_file_exists(self, context):
        adapter = context.adapter()
        disk = adapter.add_disk()
        adapter.add_parameter('fc_host', {'node_name': 'ignored'})

        self.assertTrue(os.path.exists('/sys/class/fc_host/host0/node_name'))

    @testlib.with_context
    def test_add_parameter_parameter_file_contents(self, context):
        adapter = context.adapter()
        disk = adapter.add_disk()
        adapter.add_parameter('fc_host', {'node_name': 'value'})

        param_file = open('/sys/class/fc_host/host0/node_name')
        param_value = param_file.read()
        param_file.close()

        self.assertEquals('value', param_value)

    @testlib.with_context
    def test_uname_explicitly_defined(self, context):
        context.kernel_version = 'HELLO'
        import os

        result = os.uname()

        self.assertEquals('HELLO', result[2])

    @testlib.with_context
    def test_uname_default_kernel_version(self, context):
        import os

        result = os.uname()

        self.assertEquals('3.1', result[2])

    @testlib.with_context
    def test_inventory(self, context):
        context.inventory = {}

        inventory_file = open('/etc/xensource-inventory', 'rb')
        inventory = inventory_file.read()
        inventory_file.close()

        self.assertEquals('', inventory)

    @testlib.with_context
    def test_default_inventory(self, context):
        inventory_file = open('/etc/xensource-inventory', 'rb')
        inventory = inventory_file.read()
        inventory_file.close()

        self.assertEquals("PRIMARY_DISK='/dev/disk/by-id/primary'", inventory)

    @testlib.with_context
    def test_exists_returns_false_for_non_existing(self, context):
        self.assertFalse(os.path.exists('somefile'))

    @testlib.with_context
    def test_exists_returns_true_for_root(self, context):
        self.assertTrue(os.path.exists('/'))

    @testlib.with_context
    def test_error_codes_read(self, context):
        errorcodes_file = open('/opt/xensource/sm/XE_SR_ERRORCODES.xml', 'rb')
        errorcodes = errorcodes_file.read()
        errorcodes_file.close()

        self.assertTrue("<SM-errorcodes>" in errorcodes)

    @testlib.with_context
    def test_executable_shows_up_on_filesystem(self, context):
        context.add_executable('/something', None)

        self.assertTrue(os.path.exists('/something'))

    @testlib.with_context
    def test_subprocess_execution(self, context):
        context.add_executable(
            'something',
            lambda args, inp: (1, inp + ' out', ','.join(args)))
        import subprocess

        proc = subprocess.Popen(
            ['something', 'a', 'b'],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            close_fds=True)

        out, err = proc.communicate('in')
        rc = proc.returncode

        self.assertEquals(1, rc)
        self.assertEquals('in out', out)
        self.assertEquals('something,a,b', err)

    @testlib.with_context
    def test_modinfo(self, context):
        import subprocess

        proc = subprocess.Popen(
            ['/sbin/modinfo', '-d', 'somemodule'],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            close_fds=True)

        out, err = proc.communicate('in')
        rc = proc.returncode

        self.assertEquals(0, rc)
        self.assertEquals('somemodule-description', out)
        self.assertEquals('', err)


class TestFilesystemFor(unittest.TestCase):
    def test_returns_single_item_for_root(self):
        fs = testlib.filesystem_for('/')

        self.assertEquals(['/'], fs)

    def test_returns_multiple_items_for_path(self):
        fs = testlib.filesystem_for('/somedir')

        self.assertEquals(['/', '/somedir'], fs)
