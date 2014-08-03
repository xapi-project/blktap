#!/usr/bin/python

import unittest
import tempfile
import shutil
import subprocess
import os
import errno

# FIXME provide a way to specify the working directory, or at least autodetect
# it
vhd_util = '../vhd/vhd-util'

class TestVHD(unittest.TestCase):
	DEFAULT_SIZE = 16
	MAX_SIZE_OLD = 2093056 # 2044 GB
	MAX_SIZE_NEW = 16744478

	@classmethod
	def attach(cls, name):
		cmd = ['tap-ctl', 'create', '-a', 'vhd:/%s' % name]
		rc = cls._exec(cmd)
		if rc != 0:
			raise Exception('failed to create tapdisk: %s' % cls.output)
		return cls.output

	@classmethod
	def detach(cls, name):
		cmd = ['tap-ctl', 'list']
		rc = cls._exec(cmd)
		if rc != 0:
			raise Exception('failed to list tapdisks: %s' % cls.output)
		tapdisks = cls.output.splitlines()
		for line in tapdisks:
			fields = line.split()
			assert len(fields) == 5
			if os.basename(fields[4]) == name:
				cmd = ['tap-ctl', 'destroy', '-p', fields[0], '-m', fields[1]]
				rc = cls._exec(cmd)
				if rc != 0:
					raise Exception('failed to destroy tapdisk[%s]: %s' \
							% (fields[0], cls.output))
				return
		raise Exception('tapdisk not found')

	@classmethod
	def setUpClass(cls):
		cls.workspace = tempfile.mkdtemp()

	@classmethod
	def tearDownClass(cls):
		shutil.rmtree(cls.workspace)

	@classmethod
	def mkname(cls):
		return tempfile.mktemp(dir=cls.workspace) + '.vhd'

	def setUp(self):
		self.name = self.mkname()

	# FIXME can we call the vhd_XXX methods using decorators?
	@classmethod
	def _exec(cls, cmd):
		cls.output = None
		try:
			cls.output = subprocess.check_output(cmd)
			return 0
		except subprocess.CalledProcessError, e:
			cls.output = e.output
			return e.returncode

	@classmethod
	def vhd_create(cls, name, size=DEFAULT_SIZE, reserve=None, mtdt_size=None,
			large=None):
		cmd = [vhd_util, 'create', '-n', name, '-s', str(size)]
		if reserve:
			cmd.append('-r')
		if mtdt_size:
			cmd += ['-S', str(mtdt_size)]
		if large:
			cmd.append('-l')
		return cls._exec(cmd)

	@classmethod
	def vhd_check(cls, name, parents=False):
		cmd = [vhd_util, 'check', '-n', name]
		if parents:
			cmd.append('-p')
		return cls._exec(cmd)

	@classmethod
	def vhd_snapshot(cls, name, parent, depth_limit=None, raw_parent=False,
			mtdt_size=None, force_link=False):
		cmd = [vhd_util, 'snapshot', '-n', name, '-p', parent]
		if depth_limit:
			cmd += ['-l', depth_limit]
		if raw_parent:
			cmd.append('-m')
		if mtdt_size:
			cmd += ['-S', str(mtdt_size)]
		if force_link:
			cmd.append('-e')
		return cls._exec(cmd)

	@classmethod
	def vhd_resize(cls, name, new_size, fast=False, journal=None):
		cmd = [vhd_util, 'resize', '-n', name, '-s', str(new_size)]
		if fast:
			cmd.append('-f')
		if journal:
			cmd += ['-j', journal]
		return cls._exec(cmd)

	@classmethod
	def vhd_query(cls, name):
		"""Queries the VHD file and sets the output as a dictionary."""
		cmd = ['fakeroot', vhd_util, 'query', '-n', name, '-v', '-s', '-p',
				'-f', '-m', '-d', '-S']
		rc = cls._exec(cmd)
		if not rc:
			cls.output = cls.output.splitlines()
			_output = {}
			_output['virtual size'] = int(cls.output[0])
			_output['utilisation'] = int(cls.output[1])
			if cls.output[2] == name + ' has no parent':
				_output['parent'] = None
			else:
				_output['parent'] = cls.output[2]
			hidden = cls.output[3][len('hidden: ')]
			assert hidden in ['0', '1']
			if hidden == '0':
				_output['hidden'] = False
			else:
				_output['hidden'] = True
			_output['chain depth'] = \
					int((cls.output[5][len('chain depth: '):]))
			_output['max virtual size'] = int(cls.output[6])
			cls.output = _output
		return rc

class TestCreate(TestVHD):

	def test_create_old_ver(self):
		"""Create an old VHD file."""
		self.assertEqual(0, self.vhd_create(self.name))
		self.assertEqual(0, self.vhd_check(self.name))
		self.vhd_query(self.name)

		# FIXME post condition checks,  very tedious to write for each and
		# every test
		self.assertEqual(self.DEFAULT_SIZE, self.output['virtual size'])
		self.assertNotEqual(0, self.output['utilisation'])
		self.assertIsNone(self.output['parent'])
		self.assertFalse(self.output['hidden'])
		self.assertEqual(1, self.output['chain depth'])
		self.assertEqual(self.DEFAULT_SIZE, self.output['max virtual size'])

	def test_create_old_ver_prealloc(self):
		"""Create an old VHD file, preallocating metadata."""
		self.assertEqual(0, self.vhd_create(self.name, mtdt_size=20))
		self.assertEqual(0, self.vhd_check(self.name))

	def test_create_old_ver_prealloc_large(self):
		"""Attemp to create an old VHD file, preallocating metadata for a
		too large VHD."""
		self.assertNotEqual(0, self.vhd_create(self.name,
			mtdt_size=self.MAX_SIZE_OLD + 1))

	def test_create_old_ver_large(self):
		"""Attempt to create an old version VHD file larger than 2044 GB."""
		self.assertNotEqual(0, self.vhd_create(self.name,
			size=self.MAX_SIZE_OLD + 1))

	def test_create_new_ver(self):
		"""Create a new-version VHD file."""
		self.assertEqual(0, self.vhd_create(self.name,large=True))
		self.assertEqual(0, self.vhd_check(self.name))

	def test_create_new_ver(self):
		"""Create a new-version VHD file."""
		self.assertEqual(0, self.vhd_create(self.name, large=True))
		self.assertEqual(0, self.vhd_check(self.name))

	def test_create_new_ver_large_prealloc(self):
		"""Create a new-version VHD file, preallocating metadata."""
		size = self.MAX_SIZE_OLD + 1;
		mtdt_size = size + 1;
		self.assertEqual(0, self.vhd_create(self.name, size=size,
			mtdt_size=mtdt_size, large=True))
		self.assertEqual(0, self.vhd_check(self.name))

	def test_create_new_ver_large_prealloc_large(self):
		"""Attempt to create a new-version VHD file, preallocating metadata	for a too large VHD."""
		size = self.MAX_SIZE_OLD + 1;
		mtdt_size = self.MAX_SIZE_NEW + 1;
		self.assertNotEqual(0, self.vhd_create(self.name, size=size,
			mtdt_size=mtdt_size, large=True))

	def test_create_old_very_large(self):
		"""Attempt to create a very large old-version VHD file."""
		self.assertNotEqual(0, self.vhd_create(self.name,
			size=self.MAX_SIZE_NEW + 1))

	def test_create_new_very_large(self):
		"""Attempt to create a very large new-version VHD file."""
		self.assertNotEqual(0, self.vhd_create(self.name,
			size=self.MAX_SIZE_NEW + 1, large=True))

	# FIXME implement tests that read/write data

class TestSnapshot(TestVHD):

	def setUp(self):
		self.name = self.mkname()
		self.parent = self.mkname()
		self.grandparent = self.mkname()

	def test_snapshot_old(self):
		"""Take a snapshot of an old-version VHD file."""
		self.vhd_create(self.parent)
		self.assertEqual(0, self.vhd_snapshot(self.name, self.parent),
				self.output)
		self.assertEqual(0, self.vhd_check(self.name), self.output)

	def test_snapshot_old_double(self):
		"""Take a snapshot of an old-version VHD file, and the take a snapshot of the snapshot."""
		self.vhd_create(self.grandparent)
		self.assertEqual(0, self.vhd_snapshot(self.parent, self.grandparent),
				self.output)
		self.assertEqual(0, self.vhd_check(self.parent), self.output)
		self.assertEqual(0, self.vhd_snapshot(self.name, self.parent),
				self.output)
		self.assertEqual(0, self.vhd_check(self.name), self.output)

	# FIXME implement test(s):
	#	- write some data, then take snapshot
	#	- -S
	#	- -m

	def test_snapshot_new(self):
		"""Take a snapshot of a new-version VHD file."""
		self.vhd_create(self.parent, large=True)
		self.assertEqual(0, self.vhd_snapshot(self.name, self.parent),
				self.output)
		self.assertEqual(0, self.vhd_check(self.name), self.output)

	def test_snapshot_new_double(self):
		self.vhd_create(self.grandparent, large=True)
		self.assertEqual(0, self.vhd_snapshot(self.parent, self.grandparent),
				self.output)
		self.assertEqual(0, self.vhd_check(self.parent), self.output)
		self.assertEqual(0, self.vhd_snapshot(self.name, self.parent),
				self.output)
		self.assertEqual(0, self.vhd_check(self.name), self.output)

	def test_snapshot_prealloc_mtdt(self):
		"""Take a snapshot of a small VHD for which very large metadata have been pre-allocated."""
		self.vhd_create(self.grandparent, large=True)
		self.assertEqual(0, self.vhd_snapshot(self.name, self.parent,
			mtdt_size=self.MAX_SIZE_NEW), self.output)


class TestResizeOldVersion(TestVHD):

	def setUp(self):
		self.name = self.mkname()
		self.journal = tempfile.mktemp(suffix='journal', dir=self.workspace)

	def test_resize_old_fast_no_prealloc(self):
		"""Attempt to fast-resize an old-version VHD file without having preallocated space for metadata."""
		self.vhd_create(self.name)
		self.assertNotEqual(0,
				self.vhd_resize(self.name, self.DEFAULT_SIZE + 1, fast=True),
				self.output)

	def test_resize_old_fast(self):
		"""Fast-resize an old-version VHD file within limits."""
		self.vhd_create(self.name, mtdt_size=20)
		self.assertEqual(0,
				self.vhd_resize(self.name, self.DEFAULT_SIZE + 1, fast=True),
				self.output)

	def test_resize_old_fast_at_limit(self):
		"""Fast-resize an old-version VHD file using the maximum possible size."""
		self.vhd_create(self.name, mtdt_size=self.MAX_SIZE_OLD)
		self.assertEqual(0,
				self.vhd_resize(self.name, self.MAX_SIZE_OLD, fast=True),
				self.output)

	def test_resize_old_journal_at_limit(self):
		"""Journal-resize an old-version VHD file using the maximum possible size."""
		self.vhd_create(self.name)
		self.assertEqual(0,
				self.vhd_resize(self.name, self.MAX_SIZE_OLD,
					journal=self.journal), self.output)

	def test_resize_old_journal_at_limit_prealloc(self):
		"""Journal-resize an old-version VHD file with pre-allocated metadata within limits, but with pre-allocated metadata not being large enough."""
		mtdt_size = self.DEFAULT_SIZE * 2;
		self.vhd_create(self.name, mtdt_size=mtdt_size)
		self.assertEqual(0,
				self.vhd_resize(self.name, mtdt_size * 2,
					journal=self.journal), self.output)

	def test_resize_old_journal_at_limit_prealloc(self):
		"""Journal-resize an old-version VHD file with pre-allocated metadata using the maximum possible size."""
		self.vhd_create(self.name, mtdt_size=self.MAX_SIZE_OLD)
		self.assertEqual(0,
				self.vhd_resize(self.name, self.MAX_SIZE_OLD,
					journal=self.journal), self.output)

	def test_resize_old_fast_too_large(self):
		"""Attempt to fast-resize an old-version VHD file outside limits."""
		size = 20
		self.vhd_create(self.name, mtdt_size=size)
		self.assertNotEqual(0,
				self.vhd_resize(self.name, size + 1, fast=True),
				self.output)

	def test_resize_old_journal(self):
		"""Journal-resize an old-version VHD file within limits."""
		self.vhd_create(self.name)
		self.assertEqual(0,
				self.vhd_resize(self.name, self.DEFAULT_SIZE + 1,
				journal=self.journal), self.output)

	def test_resize_old_journal_too_large(self):
		"""Attempt to journal-resize an old-version VHD file outside limits."""
		self.vhd_create(self.name)
		self.assertNotEqual(0,
				self.vhd_resize(self.name, self.MAX_SIZE_OLD + 1,
				journal=self.journal), self.output)

	def test_resize_old_journal_too_large(self):
		"""Attempt to journal-resize an old-version VHD file into a too large VHD file."""
		self.vhd_create(self.name)
		self.assertNotEqual(0,
				self.vhd_resize(self.name, self.MAX_SIZE_OLD + 1, fast=True),
				self.output)

	# FIXME check revert

class TestResizeNewVersion(TestResizeOldVersion):

	@classmethod
	def vhd_create(cls, *args, **kwargs):
		assert 'large' not in kwargs
		kwargs['large'] = True
		return super(TestResizeNewVersion, cls).vhd_create(*args, **kwargs)

	@classmethod
	def setUpClass(cls):
		cls.MAX_SIZE_OLD = cls.MAX_SIZE_NEW
		super(TestResizeNewVersion, cls).setUpClass()

class TestWrite(TestVHD):

	def test_write_old(self):
		"""Write to and old VHD file within limits."""
		name = self.mkname()
		self.vhd_create(name)
		device = self.attach(name)
		try:
			pass
		finally:
			self.detach(name)

if __name__ == '__main__':
	unittest.main()
