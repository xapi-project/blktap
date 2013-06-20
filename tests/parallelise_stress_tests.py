#!/usr/bin/python

import sys, os, datetime, time

def shellquote(arg):
    return '"%s"' % arg.replace('"', '\\"')

def pread(cmdlist):
    fd = os.popen("%s &" % \
                  (' '.join([shellquote(arg) for arg in cmdlist])))
    return

t = datetime.datetime.now()
LOGBASE = "/tmp/SR-testlog-%d" % time.mktime(t.timetuple())
print "FILE: [%s]" % LOGBASE
script_args = sys.argv
script_args[0] = './test_stress_fs.sh'

print "Calling args: [%s]" % script_args
for i in range(0,2):
    LOGFILE="%s-%d" % (LOGBASE, i)
    print "\tCalling stress_test (%d), logfile %s" % (i, LOGFILE)
    script_args.append("DEBUG_FILE=%s" % LOGFILE)
    pread(script_args)
    time.sleep(20)
