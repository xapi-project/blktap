#!/usr/bin/python

import unittest
import tempfile
import shutil
import subprocess
import os

# FIXME provide a way to specify the working directory, or at least autodetect
# it
vhd_util = 'vhd/vhd-util'

class TestCreateVHD(unittest.TestCase):

	MAX_SIZE_OLD = 2044*1024
	MAX_SIZE_NEW = 16744478

	@classmethod
	def vhd_create(cls, name, size, reserve=None, mtdt_size=None, large=None):
		cmd = ['fakeroot', vhd_util, 'create', '-n', name, '-s', str(size)]
		if reserve:
			cmd.append('-r')
		if mtdt_size:
			cmd += ['-S', str(mtdt_size)]
		if large:
			cmd.append('-l')
		return subprocess.call(cmd, stdout=cls.devnull_fp)

	@classmethod
	def setUpClass(cls):
		cls.workspace = tempfile.mkdtemp()

		# For discarding subprocess output
		cls.devnull_fp = open(os.devnull, 'w')

	@classmethod
	def tearDownClass(cls):
		shutil.rmtree(cls.workspace)

	def setUp(self):
		self.name = tempfile.mktemp() + '.vhd'

	def tearDown(self):
		pass

	def test_create_old_ver(self):
		"""Create an old VHD file."""
		self.assertEqual(0, self.vhd_create(self.name, 16))

	def test_create_old_ver_prealloc(self):
		"""Create an old VHD file, preallocating metadata."""
		self.assertEqual(0, self.vhd_create(self.name, 16, mtdt_size=20))

	def test_create_old_ver_prealloc_large(self):
		"""Attemp to create an old VHD file, preallocating metadata for a
		large VHD."""
		self.assertNotEqual(0, self.vhd_create(self.name, 16,
			mtdt_size=self.MAX_SIZE_OLD + 1))

	def test_create_old_ver_large(self):
		"""Attempt to create an old version VHD file larger than 2044 GB."""
		self.assertNotEqual(0, self.vhd_create(self.name,
			self.MAX_SIZE_OLD + 1))

	def test_create_new_ver(self):
		"""Create a new-version VHD file"""
		self.assertEqual(0, self.vhd_create(self.name, 16, large=True))

	def test_create_new_ver(self):
		"""Create a new-version VHD file"""
		self.assertEqual(0, self.vhd_create(self.name, 16, large=True))

	def test_create_new_ver_large_prealloc(self):
		"""Create a new-version VHD file, preallocating metadata"""
		size = self.MAX_SIZE_OLD + 1;
		mtdt_size = size + 1;
		self.assertEqual(0, self.vhd_create(self.name, size,
			mtdt_size=mtdt_size, large=True))

	def test_create_old_very_large(self):
		"""Attempt to create a very large old-version VHD file."""
		self.assertNotEqual(0, self.vhd_create(self.name,
			self.MAX_SIZE_NEW + 1))

	def test_create_new_very_large(self):
		"""Attempt to create a very large new-version VHD file."""
		self.assertNotEqual(0, self.vhd_create(self.name,
			self.MAX_SIZE_NEW + 1, large=True))

if __name__ == '__main__':
	unittest.main()
