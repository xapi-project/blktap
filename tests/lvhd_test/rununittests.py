#!/usr/bin/python

import sys
import tutil
import unittest
import vdi_tests
import logger

def main():
    # TODO append date to file name
    logger.logger = tutil.Logger('/tmp/smunittests.log', 2)

    # XXX Put here the unit tests you want to be executed.
    testcases = [vdi_tests.VDIBasicTest]
    suites = []

    for tc in testcases:
        suites.append(unittest.TestLoader().loadTestsFromTestCase(tc))

    return 1 == unittest.TextTestRunner().run(unittest.TestSuite(suites))

if __name__ == '__main__':
    sys.exit(main())
