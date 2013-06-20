#!/usr/bin/env python
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

import os
import blktap2
import glob
import SR
from stat import * # S_ISBLK(), ...

SECTOR_SHIFT = 9

class CachingTap(object):

    def __init__(self, tapdisk, stats):
        self.tapdisk = tapdisk
        self.stats   = stats

    @classmethod
    def from_tapdisk(cls, tapdisk, stats):

        # pick the last image. if it's a VHD, we got a parent
        # cache. the leaf case is an aio node sitting on a
        # parent-caching tapdev. always checking the complementary
        # case, so we bail on unexpected chains.

        images = stats['images']
        image  = images[-1]
        path   = image['name']
        _type  = image['driver']['name']

        def __assert(cond):
            if not cond:
                raise self.NotACachingTapdisk(tapdisk, stats)

        if _type == 'vhd':
            # parent

            return ParentCachingTap(tapdisk, stats)

        elif _type == 'aio':
            # leaf
            st = os.stat(path)

            __assert(S_ISBLK(st.st_mode))

            major = os.major(st.st_rdev)
            minor = os.minor(st.st_rdev)

            __assert(major == tapdisk.major())

            return LeafCachingTap(tapdisk, stats, minor)

        __assert(0)

    class NotACachingTapdisk(Exception):

        def __init__(self, tapdisk, stats):
            self.tapdisk = tapdisk
            self.stats   = stats

        def __str__(self):
            return \
                "Tapdisk %s in state '%s' not found caching." % \
                (self.tapdisk, self.stats)

class ParentCachingTap(CachingTap):

    def __init__(self, tapdisk, stats):
        CachingTap.__init__(self, tapdisk, stats)
        self.leaves = []

    def add_leaves(self, tapdisks):
        for t in tapdisks:
            if t.is_child_of(self):
                self.leaves.append(t)

    def vdi_stats(self):
        """Parent caching hits/miss count."""

        images = self.stats['images']
        total  = self.stats['secs'][0]

        rd_Gc = images[0]['hits'][0]
        rd_lc = images[1]['hits'][0]

        rd_hits = rd_Gc
        rd_miss = total - rd_hits

        return (rd_hits, rd_miss)

    def vdi_stats_total(self):
        """VDI total stats, including leaf hits/miss counts."""

        rd_hits, rd_miss = self.vdi_stats()
        wr_rdir = 0

        for leaf in self.leaves:
            l_rd_hits, l_rd_miss, l_wr_rdir = leaf.vdi_stats()
            rd_hits += l_rd_hits
            rd_miss += l_rd_miss
            wr_rdir += l_wr_rdir

        return rd_hits, rd_miss, wr_rdir

    def __str__(self):
        return "%s(%s, minor=%s)" % \
            (self.__class__.__name__,
             self.tapdisk.path, self.tapdisk.minor)

class LeafCachingTap(CachingTap):

    def __init__(self, tapdisk, stats, parent_minor):
        CachingTap.__init__(self, tapdisk, stats)
        self.parent_minor = parent_minor

    def is_child_of(self, parent):
        return parent.tapdisk.minor == self.parent_minor

    def vdi_stats(self):
        images = self.stats['images']
        total  = self.stats['secs'][0]

        rd_Ac = images[0]['hits'][0]
        rd_A  = images[1]['hits'][0]

        rd_hits  = rd_Ac
        rd_miss  = rd_A
        wr_rdir = self.stats['FIXME_enospc_redirect_count']

        return rd_hits, rd_miss, wr_rdir

    def __str__(self):
        return "%s(%s, minor=%s)" % \
            (self.__class__.__name__,
             self.tapdisk.path, self.tapdisk.minor)

class CacheFileSR(object):

    CACHE_NODE_EXT = '.vhdcache'

    def __init__(self, sr_path):
        self.sr_path = sr_path

    def is_mounted(self):
        # NB. a basic check should do, currently only for CLI usage.
        return os.path.exists(self.sr_path)

    class NotAMountPoint(Exception):

        def __init__(self, path):
            self.path = path

        def __str__(self):
            return "Not a mount point: %s" % self.path

    @classmethod
    def from_uuid(cls, sr_uuid):
        import SR
        sr_path = "%s/%s" % (SR.MOUNT_BASE, sr_uuid)

        cache_sr = cls(sr_path)

        if not cache_sr.is_mounted():
            raise cls.NotAMountPoint(sr_path)

        return cache_sr

    @classmethod
    def from_session(cls, session):
        import util
        import SR as sm

        host_ref = util.get_localhost_uuid(session)

        _host = session.xenapi.host
        sr_ref = _host.get_local_cache_sr(host_ref)
        if not sr_ref:
            raise util.SMException("Local cache SR not specified")

        if sr_ref == 'OpaqueRef:NULL':
            raise util.SMException("Local caching not enabled.")

        _SR = session.xenapi.SR
        sr_uuid = _SR.get_uuid(sr_ref)

        target = sm.SR.from_uuid(session, sr_uuid)

        return cls(target.path)

    @classmethod
    def from_cli(cls):
        import XenAPI

        session = XenAPI.xapi_local()
        session.xenapi.login_with_password('root', '')

        return cls.from_session(session)

    def statvfs(self):
        return os.statvfs(self.sr_path)

    def _fast_find_nodes(self):
        pattern = "%s/*%s" % (self.sr_path, self.CACHE_NODE_EXT)

        found = glob.glob(pattern)

        return list(found)

    def xapi_vfs_stats(self):
        f =  self.statvfs()
        if not f.f_frsize:
            raise util.SMException("Cache FS does not report utilization.")

        fs_size = f.f_frsize * f.f_blocks
        fs_free = f.f_frsize * f.f_bfree

        fs_cache_total = 0
        for path in self._fast_find_nodes():
            st = os.stat(path)
            fs_cache_total += st.st_size

        return {
            'FREE_CACHE_SPACE_AVAILABLE':
                fs_free,
            'TOTAL_CACHE_UTILISATION':
                fs_cache_total,
            'TOTAL_UTILISATION_BY_NON_CACHE_DATA':
                fs_size - fs_free - fs_cache_total
            }

    @classmethod
    def _fast_find_tapdisks(cls):

        # NB. we're only about to gather stats here, so take the
        # fastpath, bypassing agent based VBD[currently-attached] ->
        # VDI[allow-caching] -> Tap resolution altogether. Instead, we
        # list all tapdisk and match by path suffix.

        tapdisks = []

        for tapdisk in blktap2.Tapdisk.list():
            try:
                ext = os.path.splitext(tapdisk.path)[1]
            except:
                continue

            if ext != cls.CACHE_NODE_EXT: continue

            try:
                stats = tapdisk.stats()
            except blktap2.TapCtl.CommandFailure, e:
                if e.errno != errno.ENOENT: raise
                continue # shut down

            caching = CachingTap.from_tapdisk(tapdisk, stats)
            tapdisks.append(caching)

        return tapdisks

    def fast_scan_topology(self):

        # NB. gather all tapdisks. figure out which ones are leaves
        # and which ones cache parents.

        parents = []
        leaves  = []

        for caching in self._fast_find_tapdisks():
            if type(caching) == ParentCachingTap:
                parents.append(caching)
            else:
                leaves.append(caching)

        for parent in parents:
            parent.add_leaves(leaves)

        return parents

    def vdi_stats_total(self):

        parents = self.fast_scan_topology()

        rd_hits, rd_miss, wr_rdir = 0, 0, 0

        for parent in parents:
            p_rd_hits, p_rd_miss, p_wr_rdir = parent.vdi_stats_total()
            rd_hits += p_rd_hits
            rd_miss += p_rd_miss
            wr_rdir += p_wr_rdir

        return rd_hits, rd_miss, wr_rdir

    def xapi_vdi_stats(self):
        rd_hits, rd_miss, wr_rdir = self.vdi_stats_total()

        return {
            'TOTAL_CACHE_HITS':
                rd_hits << SECTOR_SHIFT,
            'TOTAL_CACHE_MISSES':
                rd_miss << SECTOR_SHIFT,
            'TOTAL_CACHE_ENOSPACE_REDIRECTS':
                wr_rdir << SECTOR_SHIFT,
            }

    def xapi_stats(self):

        vfs = self.xapi_vfs_stats()
        vdi = self.xapi_vdi_stats()

        vfs.update(vdi)
        return vfs

CacheSR = CacheFileSR

if __name__ == '__main__':

    import sys
    from pprint import pprint

    args = list(sys.argv)
    prog = args.pop(0)
    prog = os.path.basename(prog)

    def usage(stream):
        if prog == 'tapdisk-cache-stats':
            print >>stream, \
                "usage: tapdisk-cache-stats [<sr-uuid>]"
        else:
            print >>stream, \
                "usage: %s sr.{stats|topology} [<sr-uuid>]" % prog

    def usage_error():
        usage(sys.stderr)
        sys.exit(1)

    if prog == 'tapdisk-cache-stats':
        cmd = 'sr.stats'
    else:
        try:
            cmd = args.pop(0)
        except IndexError:
            usage_error()

    try:
        _class, method = cmd.split('.')
    except:
        usage(sys.stderr)
        sys.exit(1)

    if _class == 'sr':
        try:
            uuid = args.pop(0)
        except IndexError:
            cache_sr = CacheSR.from_cli()
        else:
            cache_sr = CacheSR.from_uuid(uuid)

        if method == 'stats':

            d = cache_sr.xapi_stats()
            for item in d.iteritems():
                print "%s=%s" % item

        elif method == 'topology':
            parents = cache_sr.fast_scan_topology()

            for parent in parents:
                print parent, "hits/miss=%s total=%s" % \
                    (parent.vdi_stats(), parent.vdi_stats_total())
                pprint(parent.stats)

                for leaf in parent.leaves:
                    print leaf, "hits/miss=%s" % str(leaf.vdi_stats())
                    pprint(leaf.stats)

            print "sr.total=%s" % str(cache_sr.vdi_stats_total())

        else:
            usage_error()
    else:
        usage_error()
