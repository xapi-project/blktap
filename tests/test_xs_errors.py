import unittest

import testlib

import xs_errors


class TestXenError(unittest.TestCase):
    @testlib.with_context
    def test_without_xml_defs(self, context):
        raised_exception = None
        try:
            xs_errors.XenError('blah')
        except Exception, e:
            raised_exception = e

        self.assertTrue("No XML def file found" in str(e))

    @testlib.with_context
    def test_xml_defs(self, context):
        context.setup_error_codes()

        raised_exception = None
        try:
            xs_errors.XenError('SRInUse')
        except Exception, e:
            raised_exception = e

        self.assertTrue("The SR device is currently in use" in str(e))
