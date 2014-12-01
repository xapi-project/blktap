import optparse


class LogicalVolume(object):
    def __init__(self, vg, name, size_mb, tag, active, zeroed):
        self.name = name
        self.size_mb = size_mb
        self.volume_group = vg
        self.tag = tag
        self.active = active
        self.zeroed = zeroed


class VolumeGroup(object):
    def __init__(self, name):
        self.name = name
        self.volumes = []

    def add_volume(self, name, size_mb, tag=None, active=True, zeroed=True):
        self.volumes.append(
            LogicalVolume(self, name, size_mb, tag, active, zeroed))

    def delete_volume(self, volume):
        self.volumes = [vol for vol in self.volumes if vol != volume]


class LVSubsystem(object):
    def __init__(self, logger, executable_injector):
        self.logger = logger
        self.lv_calls = []
        self._volume_groups = []
        executable_injector('/usr/sbin/lvcreate', self.fake_lvcreate)
        executable_injector('/usr/sbin/lvremove', self.fake_lvremove)
        executable_injector('/sbin/dmsetup', self.fake_dmsetup)

    def add_volume_group(self, name):
        self._volume_groups.append(VolumeGroup(name))

    def get_logical_volumes_with_name(self, name):
        result = []
        for vg in self._volume_groups:
            for lv in vg.volumes:
                if name == lv.name:
                    result.append(lv)
        return result

    def get_volume_group(self, vgname):
        for vg in self._volume_groups:
            if vg.name == vgname:
                return vg

    def fake_lvcreate(self, args, stdin):
        self.logger('lvcreate', repr(args), stdin)
        parser = optparse.OptionParser()
        parser.add_option("-n", dest='name')
        parser.add_option("-L", dest='size_mb')
        parser.add_option("--addtag", dest='tag')
        parser.add_option("--inactive", dest='inactive', action='store_true')
        parser.add_option("--zero", dest='zero', default='y')
        try:
            options, args = parser.parse_args(args[1:])
        except SystemExit, e:
            self.logger("LVCREATE OPTION PARSING FAILED")
            return (1, '', str(e))

        vgname, = args

        if self.get_volume_group(vgname) is None:
            self.logger("volume group does not exist:", vgname)
            return (1, '', '  Volume group "%s" not found\n' % vgname)

        active = not options.inactive
        assert options.zero in ['y', 'n']
        zeroed = options.zero == 'y'

        self.get_volume_group(vgname).add_volume(
            options.name,
            int(options.size_mb),
            options.tag,
            active,
            zeroed)

        return 0, '', ''

    def fake_lvremove(self, args, stdin):
        self.logger('lvremove', repr(args), stdin)
        parser = optparse.OptionParser()
        parser.add_option(
            "-f", "--force", dest='force', action='store_true', default=False)
        self.logger(args, stdin)
        try:
            options, args = parser.parse_args(args[1:])
        except SystemExit, e:
            self.logger("LVREMOVE OPTION PARSING FAILED")
            return (1, '', str(e))

        lvpath, = args

        for vg in self._volume_groups:
            for lv in vg.volumes:
                if '/'.join([vg.name, lv.name]) == lvpath:
                    vg.delete_volume(lv)

        return 0, '', ''

    def fake_dmsetup(self, args, stdin):
        return 0, '', ''
