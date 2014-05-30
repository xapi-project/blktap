import re
import mock
import os
import StringIO
import fnmatch
import string
import random
import textwrap


PATHSEP = '/'


def get_error_codes():
    this_dir = os.path.dirname(__file__)
    drivers_dir = os.path.join(this_dir, '..', 'drivers')
    error_codes_path = os.path.join(drivers_dir, 'XE_SR_ERRORCODES.xml')
    error_code_catalog = open(error_codes_path, 'r')
    contents = error_code_catalog.read()
    error_code_catalog.close()
    return contents


class SCSIDisk(object):
    def __init__(self):
        self.long_id = ''.join(
            random.choice(string.digits) for _ in range(33))


class SCSIAdapter(object):
    def __init__(self):
        self.disks = []
        self.long_id = ''.join(
            random.choice(string.digits) for _ in range(33))
        self.parameters = []

    def add_disk(self):
        disk = SCSIDisk()
        self.disks.append(disk)
        return disk

    def add_parameter(self, host_class, values):
        self.parameters.append((host_class, values))


class Executable(object):
    def __init__(self, function_to_call):
        self.function_to_call = function_to_call

    def run(self, args, stdin):
        (return_code, stdout, stderr) = self.function_to_call(args, stdin)
        return (return_code, stdout, stderr)


class Subprocess(object):
    def __init__(self, executable, args):
        self.executable = executable
        self.args = args

    def communicate(self, data):
        self.returncode, out, err = self.executable.run(self.args, data)
        return out, err


class TestContext(object):
    def __init__(self):
        self.patchers = []
        self.error_codes = get_error_codes()
        self.inventory = {
            'PRIMARY_DISK': '/dev/disk/by-id/primary'
        }
        self.scsi_adapters = []
        self.kernel_version = '3.1'
        self.executables = {}

    def add_executable(self, fpath, funct):
        self.executables[fpath] = Executable(funct)

    def generate_inventory_contents(self):
        return '\n'.join(
            [
                '='.join(
                    [k, v.join(2 * ["'"])]) for k, v in self.inventory.items()
            ]
        )

    def start(self):
        self.patchers = [
            mock.patch('__builtin__.open', new=self.fake_open),
            mock.patch('os.path.exists', new=self.fake_exists),
            mock.patch('os.listdir', new=self.fake_listdir),
            mock.patch('glob.glob', new=self.fake_glob),
            mock.patch('os.uname', new=self.fake_uname),
            mock.patch('subprocess.Popen', new=self.fake_popen),
        ]
        map(lambda patcher: patcher.start(), self.patchers)
        self.setup_modinfo()

    def setup_modinfo(self):
        self.add_executable('/sbin/modinfo', self.fake_modinfo)

    def fake_modinfo(self, args, stdin_data):
        assert len(args) == 3
        assert args[1] == '-d'
        return (0, args[2] + '-description', '')

    def fake_popen(self, args, stdin, stdout, stderr, close_fds):
        import subprocess
        assert stdin == subprocess.PIPE
        assert stdout == subprocess.PIPE
        assert stderr == subprocess.PIPE
        assert close_fds is True

        path_to_executable = args[0]

        if path_to_executable not in self.executables:
            raise AssertionError(
                path_to_executable
                + ' was not found. Set it up using add_executable.'
                + ' was called with: ' + str(args))

        executable = self.executables[path_to_executable]
        return Subprocess(executable, args)

    def fake_uname(self):
        return (
            'Linux',
            'testbox',
            self.kernel_version,
            '#1 SMP Thu May 8 09:50:50 EDT 2014',
            'x86_64'
        )

    def fake_open(self, fname, mode='r'):
        if fname == '/etc/xensource-inventory':
            return StringIO.StringIO(self.generate_inventory_contents())

        elif fname == '/opt/xensource/sm/XE_SR_ERRORCODES.xml':
            return StringIO.StringIO(self.error_codes)

        for fpath, contents in self.generate_path_content():
            if fpath == fname:
                return StringIO.StringIO(contents)

        self.log('tried to open file', fname)
        raise IOError(fname)

    def fake_exists(self, fname):
        for existing_fname in self.get_filesystem():
            if fname == existing_fname:
                return True

        self.log('not exists', fname)
        return False

    def fake_listdir(self, path):
        assert '*' not in path
        glob_pattern = path + '/*'
        glob_matches = self.fake_glob(glob_pattern)
        return [match[len(path)+1:] for match in glob_matches]

    def get_filesystem(self):
        result = set(['/'])
        for devpath in self.generate_device_paths():
            for path in filesystem_for(devpath):
                result.add(path)

        for executable_path in self.executables:
            for path in filesystem_for(executable_path):
                result.add(path)

        return sorted(result)

    def generate_path_content(self):
        for host_id, adapter in enumerate(self.scsi_adapters):
            for host_class, values in adapter.parameters:
                for key, value in values.iteritems():
                    path = '/sys/class/%s/host%s/%s' % (
                        host_class, host_id, key)
                    yield (path, value)

    def generate_device_paths(self):
        actual_disk_letter = 'a'
        for host_id, adapter in enumerate(self.scsi_adapters):
            yield '/sys/class/scsi_host/host%s' % host_id
            for disk_id, disk in enumerate(adapter.disks):
                yield '/sys/class/scsi_disk/%s:0:%s:0' % (
                    host_id, disk_id)

                yield '/sys/class/scsi_disk/%s:0:%s:0/device/block/sd%s' % (
                    host_id, disk_id, actual_disk_letter)

                actual_disk_letter = chr(ord(actual_disk_letter) + 1)

                yield '/dev/disk/by-scsibus/%s-%s:0:%s:0' % (
                    adapter.long_id, host_id, disk_id)

                yield '/dev/disk/by-id/%s' % (disk.long_id)

        for path, _content in self.generate_path_content():
            yield path

    def fake_glob(self, pattern):
        result = []
        pattern_parts = pattern.split(PATHSEP)
        for fname in self.get_filesystem():
            fname_parts = fname.split(PATHSEP)
            if len(fname_parts) != len(pattern_parts):
                continue

            found = True
            for pattern_part, fname_part in zip(pattern_parts, fname_parts):
                if not fnmatch.fnmatch(fname_part, pattern_part):
                    found = False
            if found:
                result.append(fname)

        if not result:
            self.log('no glob', pattern)
        return list(set(result))

    def log(self, *args):
        WARNING = '\033[93m'
        ENDC = '\033[0m'
        import sys
        sys.stderr.write(
            WARNING
            + ' '.join(str(arg) for arg in args)
            + ENDC
            + '\n')

    def stop(self):
        map(lambda patcher: patcher.stop(), self.patchers)

    def adapter(self):
        adapter = SCSIAdapter()
        self.scsi_adapters.append(adapter)
        return adapter


def with_context(func):
    def decorated(self, *args, **kwargs):
        context = TestContext()
        context.start()
        try:
            result = func(self, context, *args, **kwargs)
            context.stop()
            return result
        except:
            context.stop()
            raise

    decorated.__name__ = func.__name__
    return decorated


def xml_string(text):
    dedented = textwrap.dedent(text).strip()
    lines = []
    for line in dedented.split('\n'):
        lines.append(re.sub(r'^ *', '', line))

    return ''.join(lines)


def marshalled(dom):
    text = dom.toxml()
    result = text.replace('\n', '')
    result = result.replace('\t', '')
    return result


def filesystem_for(path):
    result = [PATHSEP]
    assert path.startswith(PATHSEP)
    segments = [seg for seg in path.split(PATHSEP) if seg]
    for i in range(len(segments)):
        result.append(PATHSEP + PATHSEP.join(segments[:i+1]))
    return result


class XmlMixIn(object):
    def assertXML(self, expected, actual):
        import xml

        expected_dom = xml.dom.minidom.parseString(
            xml_string(expected))

        actual_dom = xml.dom.minidom.parseString(actual)

        self.assertEquals(
            marshalled(expected_dom),
            marshalled(actual_dom)
        )
