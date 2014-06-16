import unittest
import trim_util
import testlib

import mock


class AlwaysBusyLock(object):
    def acquireNoblock(self):
        return False


class AlwaysFreeLock(object):
    def __init__(self):
        self.acquired = False

    def acquireNoblock(self):
        self.acquired = True
        return True

    def release(self):
        self.acquired = False


class TestTrimUtil(unittest.TestCase, testlib.XmlMixIn):
    @mock.patch('util.sr_get_capability')
    @testlib.with_context
    def test_do_trim_error_code_trim_not_supported(self,
                                                   context,
                                                   sr_get_capability):
        sr_get_capability.return_value = []
        context.setup_error_codes()

        result = trim_util.do_trim(None, {'sr_uuid': 'some-uuid'})

        self.assertXML("""
        <?xml version="1.0" ?>
        <trim_response>
            <key_value_pair>
                <key>errcode</key>
                <value>UnsupportedSRForTrim</value>
            </key_value_pair>
            <key_value_pair>
                <key>errmsg</key>
                <value>Trim on [some-uuid] not supported</value>
            </key_value_pair>
        </trim_response>
        """, result)

    @mock.patch('time.sleep')
    @mock.patch('lock.Lock')
    @mock.patch('util.sr_get_capability')
    @testlib.with_context
    def test_do_trim_unable_to_obtain_lock_on_sr(self,
                                                 context,
                                                 sr_get_capability,
                                                 MockLock,
                                                 sleep):
        MockLock.return_value = AlwaysBusyLock()
        sr_get_capability.return_value = [trim_util.TRIM_CAP]
        context.setup_error_codes()

        result = trim_util.do_trim(None, {'sr_uuid': 'some-uuid'})

        self.assertXML("""
        <?xml version="1.0" ?>
        <trim_response>
            <key_value_pair>
                <key>errcode</key>
                <value>SRUnavailable</value>
            </key_value_pair>
            <key_value_pair>
                <key>errmsg</key>
                <value>Unable to get SR lock [some-uuid]</value>
            </key_value_pair>
        </trim_response>
        """, result)

    @mock.patch('time.sleep')
    @mock.patch('lock.Lock')
    @mock.patch('util.sr_get_capability')
    @testlib.with_context
    def test_do_trim_sleeps_a_sec_and_retries_three_times(self,
                                                          context,
                                                          sr_get_capability,
                                                          MockLock,
                                                          sleep):
        MockLock.return_value = AlwaysBusyLock()
        sr_get_capability.return_value = [trim_util.TRIM_CAP]
        context.setup_error_codes()

        trim_util.do_trim(None, {'sr_uuid': 'some-uuid'})

        self.assertEquals([
                mock.call(1),
                mock.call(1),
                mock.call(1)
            ],
            sleep.mock_calls
        )

    @mock.patch('trim_util.lvutil')
    @mock.patch('lock.Lock')
    @mock.patch('util.sr_get_capability')
    @testlib.with_context
    def test_do_trim_creates_an_lv(self,
                                   context,
                                   sr_get_capability,
                                   MockLock,
                                   lvutil):
        MockLock.return_value = AlwaysFreeLock()
        sr_get_capability.return_value = [trim_util.TRIM_CAP]
        context.setup_error_codes()

        trim_util.do_trim(None, {'sr_uuid': 'some-uuid'})

        lvutil.create.assert_called_once_with(
            'some-uuid_trim_lv', 0, 'VG_XenStorage-some-uuid',
            size_in_percentage='100%F', activate=True
        )

    @mock.patch('trim_util.lvutil')
    @mock.patch('lock.Lock')
    @mock.patch('util.sr_get_capability')
    @testlib.with_context
    def test_do_trim_removes_lv_no_leftover_trim_vol(self,
                                                     context,
                                                     sr_get_capability,
                                                     MockLock,
                                                     lvutil):
        lvutil.exists.return_value = False
        MockLock.return_value = AlwaysFreeLock()
        sr_get_capability.return_value = [trim_util.TRIM_CAP]
        context.setup_error_codes()

        trim_util.do_trim(None, {'sr_uuid': 'some-uuid'})

        lvutil.remove.assert_called_once_with(
            '/dev/VG_XenStorage-some-uuid/some-uuid_trim_lv',
            config_param='issue_discards=1')

    @mock.patch('trim_util.lvutil')
    @mock.patch('lock.Lock')
    @mock.patch('util.sr_get_capability')
    @testlib.with_context
    def test_do_trim_releases_lock(self,
                                   context,
                                   sr_get_capability,
                                   MockLock,
                                   lvutil):
        lvutil.exists.return_value = False
        sr_lock = MockLock.return_value = AlwaysFreeLock()
        sr_get_capability.return_value = [trim_util.TRIM_CAP]
        context.setup_error_codes()

        trim_util.do_trim(None, {'sr_uuid': 'some-uuid'})

        self.assertFalse(sr_lock.acquired)

    @mock.patch('trim_util.lvutil')
    @mock.patch('lock.Lock')
    @mock.patch('util.sr_get_capability')
    @testlib.with_context
    def test_do_trim_removes_lv_with_leftover_trim_vol(self,
                                                      context,
                                                      sr_get_capability,
                                                      MockLock,
                                                      lvutil):
        lvutil.exists.return_value = True
        MockLock.return_value = AlwaysFreeLock()
        sr_get_capability.return_value = [trim_util.TRIM_CAP]
        context.setup_error_codes()

        trim_util.do_trim(None, {'sr_uuid': 'some-uuid'})

        self.assertEquals([
                mock.call('/dev/VG_XenStorage-some-uuid/some-uuid_trim_lv'),
                mock.call(
                    '/dev/VG_XenStorage-some-uuid/some-uuid_trim_lv',
                    config_param='issue_discards=1')
            ], lvutil.remove.mock_calls)

    @mock.patch('trim_util.lvutil')
    @mock.patch('lock.Lock')
    @mock.patch('util.sr_get_capability')
    @testlib.with_context
    def test_do_trim_lock_released_even_if_exception_raised(self,
                                                            context,
                                                            sr_get_capability,
                                                            MockLock,
                                                            lvutil):
        lvutil.exists.side_effect = Exception('blah')
        srlock = AlwaysFreeLock()
        MockLock.return_value = srlock
        sr_get_capability.return_value = [trim_util.TRIM_CAP]
        context.setup_error_codes()

        trim_util.do_trim(None, {'sr_uuid': 'some-uuid'})

        self.assertFalse(srlock.acquired)

    @mock.patch('trim_util.lvutil')
    @mock.patch('lock.Lock')
    @mock.patch('util.sr_get_capability')
    @testlib.with_context
    def test_do_trim_when_exception_then_returns_generic_err(self,
                                                             context,
                                                             sr_get_capability,
                                                             MockLock,
                                                             lvutil):
        lvutil.exists.side_effect = Exception('blah')
        srlock = AlwaysFreeLock()
        MockLock.return_value = srlock
        sr_get_capability.return_value = [trim_util.TRIM_CAP]
        context.setup_error_codes()

        result = trim_util.do_trim(None, {'sr_uuid': 'some-uuid'})

        self.assertXML("""
        <?xml version="1.0" ?>
        <trim_response>
            <key_value_pair>
                <key>errcode</key>
                <value>UnknownTrimException</value>
            </key_value_pair>
            <key_value_pair>
                <key>errmsg</key>
                <value>Unknown Exception: trim failed on SR [some-uuid]</value>
            </key_value_pair>
        </trim_response>
        """, result)

    @mock.patch('trim_util.lvutil')
    @mock.patch('lock.Lock')
    @mock.patch('util.sr_get_capability')
    @testlib.with_context
    def test_do_trim_when_trim_succeeded_returns_true(self,
                                                      context,
                                                      sr_get_capability,
                                                      MockLock,
                                                      lvutil):
        MockLock.return_value = AlwaysFreeLock()
        sr_get_capability.return_value = [trim_util.TRIM_CAP]
        context.setup_error_codes()

        result = trim_util.do_trim(None, {'sr_uuid': 'some-uuid'})

        self.assertEquals('True', result)
