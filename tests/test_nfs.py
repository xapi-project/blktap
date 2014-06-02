import unittest
import nfs
import mock
import sys


class Test_nfs(unittest.TestCase):

    @mock.patch('util.pread')
    def test_check_server_tcp(self, pread):
        nfs.check_server_tcp('aServer')

        pread.assert_called_once_with(['/usr/sbin/rpcinfo', '-t', 'aServer',
                                      'nfs', '3'])

    def get_soft_mount_pread(self, binary):
        return ([binary, 'remoteserver:remotepath', 'mountpoint', '-o',
                 'soft,timeo=600,retrans=2147483647,transport,acdirmin=0,'
                 'acdirmax=0'])

    @mock.patch('util.makedirs')
    @mock.patch('util.pread')
    def test_soft_mount(self, pread, makedirs):
        nfs.soft_mount('mountpoint', 'remoteserver', 'remotepath', 'transport',
                       timeout=0)

        pread.assert_called_once_with(self.get_soft_mount_pread('mount.nfs'))
