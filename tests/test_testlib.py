import unittest
import os
import mock
import errno

import testlib


class TestTestContext(unittest.TestCase):
    def test_generate_inventory_file(self):
        context = testlib.TestContext()
        context.inventory = dict(key='value')

        self.assertEquals("key='value'", context.generate_inventory_contents())

    @testlib.with_context
    def test_adapter_adds_scsi_host_entry(self, context):
        context.add_adapter(testlib.SCSIAdapter())

        self.assertEquals(['host0'], os.listdir('/sys/class/scsi_host'))

    @testlib.with_context
    def test_add_disk_adds_scsi_disk_entry(self, context):
        import glob
        adapter = context.add_adapter(testlib.SCSIAdapter())
        adapter.add_disk()

        self.assertEquals(
            ['/sys/class/scsi_disk/0:0:0:0'],
            glob.glob('/sys/class/scsi_disk/0*'))

    @testlib.with_context
    def test_add_disk_adds_scsibus_entry(self, context):
        import glob
        adapter = context.add_adapter(testlib.SCSIAdapter())
        adapter.long_id = 'HELLO'
        adapter.add_disk()

        self.assertEquals(
            ['/dev/disk/by-scsibus/HELLO-0:0:0:0'],
            glob.glob('/dev/disk/by-scsibus/*'))

    @testlib.with_context
    def test_add_disk_adds_device(self, context):
        adapter = context.add_adapter(testlib.SCSIAdapter())
        adapter.add_disk()

        self.assertEquals(
            ['sda'],
            os.listdir('/sys/class/scsi_disk/0:0:0:0/device/block'))

    @testlib.with_context
    def test_add_disk_adds_disk_by_id_entry(self, context):
        adapter = context.add_adapter(testlib.SCSIAdapter())
        disk = adapter.add_disk()
        disk.long_id = 'SOMEID'

        self.assertEquals(['SOMEID'], os.listdir('/dev/disk/by-id'))

    @testlib.with_context
    def test_add_disk_adds_glob(self, context):
        import glob
        adapter = context.add_adapter(testlib.SCSIAdapter())
        disk = adapter.add_disk()

        self.assertEquals(['/dev/disk/by-id'], glob.glob('/dev/disk/by-id'))

    @testlib.with_context
    def test_add_disk_path_exists(self, context):
        adapter = context.add_adapter(testlib.SCSIAdapter())
        disk = adapter.add_disk()

        self.assertTrue(os.path.exists('/dev/disk/by-id'))

    @testlib.with_context
    def test_add_parameter_parameter_file_exists(self, context):
        adapter = context.add_adapter(testlib.SCSIAdapter())
        disk = adapter.add_disk()
        adapter.add_parameter('fc_host', {'node_name': 'ignored'})

        self.assertTrue(os.path.exists('/sys/class/fc_host/host0/node_name'))

    @testlib.with_context
    def test_add_parameter_parameter_file_contents(self, context):
        adapter = context.add_adapter(testlib.SCSIAdapter())
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
        context.setup_error_codes()
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

    @testlib.with_context
    def test_makedirs_mocked_out(self, context):
        import os

        os.makedirs('/blah/subdir')

        self.assertTrue(os.path.exists('/blah/subdir'))

    @testlib.with_context
    def test_makedirs_raises_if_exists(self, context):
        import os

        os.makedirs('/blah/subdir')

        self.assertRaises(OSError, os.makedirs, '/blah/subdir')

    @testlib.with_context
    def test_setup_error_codes(self, context):
        context.setup_error_codes()

        self.assertTrue(
            os.path.exists('/opt/xensource/sm/XE_SR_ERRORCODES.xml'))

    @testlib.with_context
    def test_write_a_file(self, context):
        import os

        os.makedirs('/blah/subdir')

        f = open('/blah/subdir/somefile', 'w+')
        f.write('hello')
        f.close()

        self.assertTrue(
            ('/blah/subdir/somefile', 'hello')
            in list(context.generate_path_content()))

    @testlib.with_context
    def test_write_a_file_in_non_existing_dir(self, context):
        import os

        try:
            open('/blah/subdir/somefile', 'w')
            raise AssertionError('No exception raised')
        except IOError, e:
            self.assertEquals(errno.ENOENT, e.errno)

    @testlib.with_context
    def test_file_returns_an_object_with_fileno_callable(self, context):
        f = file('/file', 'w+')

        self.assertTrue(hasattr(f, 'fileno'))
        self.assertTrue(callable(f.fileno))

    @testlib.with_context
    def test_filenos_are_unique(self, context):
        import os

        os.makedirs('/blah/subdir')

        file_1 = file('/blah/subdir/somefile', 'w+')
        fileno_1 = file_1.fileno()

        file_2 = file('/blah/subdir/somefile2', 'w+')
        fileno_2 = file_2.fileno()

        self.assertTrue(fileno_1 != fileno_2)

    def test_get_created_directories(self):
        context = testlib.TestContext()

        context.fake_makedirs('/some/path')

        self.assertEquals([
            '/',
            '/some',
            '/some/path'],
            context.get_created_directories())

    def test_popen_raises_error(self):
        import subprocess
        context = testlib.TestContext()

        self.assertRaises(
            testlib.ContextSetupError,
            context.fake_popen,
            ['something'],
            subprocess.PIPE,
            subprocess.PIPE,
            subprocess.PIPE,
            True
        )

    def test_glob_requests_logged(self):
        context = testlib.TestContext()
        context.log = mock.Mock()

        context.fake_glob('/dir/*')

        self.assertEquals(
            [
                mock.call('no glob', '/dir/*'),
            ],
            context.log.mock_calls
        )

    def test_fake_open_logged(self):
        context = testlib.TestContext()
        context.log = mock.Mock()

        try:
            context.fake_open('/nonexisting_file', 'r')
        except:
            pass

        self.assertEquals(
            [
                mock.call('tried to open file', '/nonexisting_file'),
            ],
            context.log.mock_calls
        )

    def test_context_stops_mocking_on_failures(self):
        original_open = os.open

        @testlib.with_context
        def somefunction(firstparam, context):
            raise Exception()

        try:
            somefunction(None)
        except:
            pass

        self.assertEquals(original_open, os.open)

    @testlib.with_context
    def test_rmdir_is_replaced_with_a_fake(self, context):
        self.assertEquals(context.fake_rmdir, os.rmdir)

    def test_rmdir_raises_error_if_dir_not_found(self):
        context = testlib.TestContext()

        try:
            context.fake_rmdir('nonexisting')
            raise AssertionError('No Exception raised')
        except OSError, e:
            self.assertEquals(errno.ENOENT, e.errno)

    def test_rmdir_removes_dir_if_found(self):
        context = testlib.TestContext()

        context.fake_makedirs('/existing_dir')

        context.fake_rmdir('/existing_dir')

        self.assertFalse(context.fake_exists('/existing_dir'))

    def test_rmdir_raises_exception_if_dir_is_not_empty(self):
        context = testlib.TestContext()

        context.fake_makedirs('/existing_dir/somefile')

        try:
            context.fake_rmdir('/existing_dir')
            raise AssertionError('No Exception raised')
        except OSError, e:
            self.assertEquals(errno.ENOTEMPTY, e.errno)


class TestFilesystemFor(unittest.TestCase):
    def test_returns_single_item_for_root(self):
        fs = testlib.filesystem_for('/')

        self.assertEquals(['/'], fs)

    def test_returns_multiple_items_for_path(self):
        fs = testlib.filesystem_for('/somedir')

        self.assertEquals(['/', '/somedir'], fs)


class TestXmlMixIn(unittest.TestCase, testlib.XmlMixIn):

    def test_assertXML_doesn_t_care_about_spaces(self):
        self.assertXML(
            """

            <?xml version="1.0" ?>
                <something/>

            """,
            '<?xml version="1.0" ?><something/>')
