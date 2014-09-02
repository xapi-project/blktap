import unittest
import mock

import cleanup

import util


class FakeXapi(object):
    def __init__(self):
        self.srRecord = {
            'name_label': 'dummy'
        }

    def isPluggedHere(self):
        return True

    def isMaster(self):
        return True


class AlwaysLockedLock(object):
    def acquireNoblock(self):
        return False


class AlwaysFreeLock(object):
    def acquireNoblock(self):
        return True


class IrrelevantLock(object):
    pass


def create_cleanup_sr():
    xapi = FakeXapi()
    return cleanup.SR(uuid=None, xapi=xapi, createLock=False, force=False)


class TestSR(unittest.TestCase):
    def setUp(self):
        self.sleep_patcher = mock.patch('cleanup.time.sleep')
        self.sleep_patcher.start()

    def tearDown(self):
        self.sleep_patcher.stop()

    def setup_abort_flag(self, ipc_mock, should_abort=False):
        flag = mock.Mock()
        flag.test = mock.Mock(return_value=should_abort)

        ipc_mock.return_value = flag

    def test_lock_if_already_locked(self):
        """
        Given an already locked SR, a lock call increments the lock counter
        """

        sr = create_cleanup_sr()
        sr._srLock = IrrelevantLock()
        sr._locked = 1

        sr.lock()

        self.assertEquals(2, sr._locked)

    def test_lock_if_no_locking_is_used(self):
        """
        Given no srLock present, the lock operations don't touch the counter
        """

        sr = create_cleanup_sr()
        sr._srLock = None

        sr.lock()

        self.assertEquals(0, sr._locked)

    @mock.patch('cleanup.IPCFlag')
    def test_lock_succeeds_if_lock_is_acquired(
            self,
            mock_ipc_flag):
        """
        After performing a lock, the counter equals to 1
        """

        self.setup_abort_flag(mock_ipc_flag)
        sr = create_cleanup_sr()
        sr._srLock = AlwaysFreeLock()

        sr.lock()

        self.assertEquals(1, sr._locked)

    @mock.patch('cleanup.IPCFlag')
    def test_lock_raises_exception_if_abort_requested(
            self,
            mock_ipc_flag):
        """
        If IPC abort was requested, lock raises AbortException
        """

        self.setup_abort_flag(mock_ipc_flag, should_abort=True)
        sr = create_cleanup_sr()
        sr._srLock = AlwaysLockedLock()

        self.assertRaises(cleanup.AbortException, sr.lock)

    @mock.patch('cleanup.IPCFlag')
    def test_lock_raises_exception_if_unable_to_acquire_lock(
            self,
            mock_ipc_flag):
        """
        If the lock is busy, SMException is raised
        """

        self.setup_abort_flag(mock_ipc_flag)
        sr = create_cleanup_sr()
        sr._srLock = AlwaysLockedLock()

        self.assertRaises(util.SMException, sr.lock)

    @mock.patch('cleanup.IPCFlag')
    def test_lock_leaves_sr_consistent_if_unable_to_acquire_lock(
            self,
            mock_ipc_flag):
        """
        If the lock is busy, the lock counter is not incremented
        """

        self.setup_abort_flag(mock_ipc_flag)
        sr = create_cleanup_sr()
        sr._srLock = AlwaysLockedLock()

        try:
            sr.lock()
        except:
            pass

        self.assertEquals(0, sr._locked)
