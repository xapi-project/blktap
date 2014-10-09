import unittest
import mock

import SRCommand


class SomeException(Exception):
    pass


class TestStandaloneFunctions(unittest.TestCase):

    @mock.patch('util.SMlog')
    @mock.patch('__builtin__.reduce')
    @mock.patch('SRCommand.SRCommand.run_statics')
    @mock.patch('SRCommand.SRCommand.parse')
    def test_run_correctly_log_all_exceptions(
            self,
            mock_parse,
            mock_run_statics,
            mock_reduce,
            mock_SMlog):

        """ Assert that any arbitrary exception raised and with a big
            message length is logged to SMlog. Only the first line of
            the message is asserted (traceback ommited).
        """

        from random import choice
        from string import ascii_letters
        from DummySR import DRIVER_INFO

        MSG_LEN = 2048

        # TestSRCommand data member to hold SMlog output.
        self.smlog_out = None

        # Generate random exception message of MSG_LEN characters
        rand_huge_msg = ''.join(choice(ascii_letters) for _ in range(MSG_LEN))

        # Create function to raise exception in SRCommand.run()
        mock_driver = mock.Mock(side_effect=SomeException(rand_huge_msg))

        # MockSMlog replaces util.SMlog. Instead of printing to
        # /var/log/SMlog, it writes the output to self.smlog_out.
        def MockSMlog(str_arg):
            self.smlog_out = str_arg.strip()

        mock_reduce.return_value = ''
        mock_SMlog.side_effect = MockSMlog

        try:
            SRCommand.run(mock_driver, DRIVER_INFO)
        except SomeException:
            # SomeException needs to be suppressed for this
            # test, as it is re-raised after it is logged.
            pass

        self.assertEqual(self.smlog_out,
                        ('***** dummy: EXCEPTION test_SRCommand.'
                         'SomeException, ' + rand_huge_msg))

    @mock.patch('util.logException')
    @mock.patch('SRCommand.SRCommand.run_statics')
    @mock.patch('SRCommand.SRCommand.parse')
    def test_run_print_xml_error_if_SRException(
            self,
            mock_parse,
            mock_run_statics,
            mock_logException):

        """ If an SR.SRException is thrown, assert that
            "print <SR.SRException instance>.toxml()" is called.
        """

        import sys
        from StringIO import StringIO
        from SR import SRException
        from DummySR import DRIVER_INFO

        # Save original sys.stdout file object.
        saved_stdout = sys.stdout

        # Create a mock_stdout object and assign it to sys.stdout
        mock_stdout = StringIO()
        sys.stdout = mock_stdout

        # Create function to raise exception in SRCommand.run()
        mock_driver = mock.Mock(side_effect=SRException(
                                "[UnitTest] SRException thrown"))

        try:
            SRCommand.run(mock_driver, DRIVER_INFO)
        except SystemExit:
            pass

        # Write SRCommand.run() output to variable.
        actual_out = mock_stdout.getvalue()

        # Restore the original sys.stdout object.
        sys.stdout = saved_stdout

        expected_out = ("<?xml version='1.0'?>\n<methodResponse>\n<fault>\n"
                        "<value><struct>\n<member>\n<name>faultCode</name>\n"
                        "<value><int>22</int></value>\n</member>\n<member>\n"
                        "<name>faultString</name>\n<value><string>[UnitTest] "
                        "SRException thrown</string></value>\n</member>\n"
                        "</struct></value>\n</fault>\n</methodResponse>\n\n")

        self.assertEqual(actual_out, expected_out)

    @mock.patch('util.logException')
    @mock.patch('SRCommand.SRCommand.run_statics')
    @mock.patch('SRCommand.SRCommand.parse')
    def test_run_reraise_if_not_SRException(
            self,
            mock_parse,
            mock_run_statics,
            mock_logException):

        """ If an exception other than SR.SRException
            is thrown, assert that it is re-raised.
        """

        from DummySR import DRIVER_INFO

        # Create function to raise exception in SRCommand.run()
        mock_driver = mock.Mock(side_effect=SomeException)

        try:
            SRCommand.run(mock_driver, DRIVER_INFO)
        except Exception, e:
            self.assertTrue(isinstance(e, SomeException))
