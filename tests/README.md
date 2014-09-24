# Unit tests for blktap

We are using the ceedling unit testing framework to test blktap. The test files
are kept in this directory, and put in place by scripts, which also run
ceedling. The ceedling project provides a make-like build framework, and uses
CMock to generate mocks, and the Unity testing framework internally.

The test framework (the ceedling project) lives in a different repository:

    https://github.com/matelakat/storage-ci

The repository will create an isolated ubuntu installation so that you don't
need to pollute your work machine. The chroot environment will be put to a
workspace. To run all the tests:

First create an empty directory that will hold your workspace:

    mkdir [path outside of blktap sources]

I will refer to that path as `workspace`. To run the tests:

    [path to storage-ci]/check-bktap.sh [path to blktap sources] [workspace]

On the next runs the workspace will be re-used:

    [path to storage-ci]/check-bktap.sh [path to the blktap sources] [workspace]

If you only want to run the unit tests, define `ONLY_UNITTESTS`:

    ONLY_UNITTESTS=yes [path to storage-ci]/check-bktap.sh [path to the blktap sources] [workspace]

For more information:

 * [CMock documentation](http://throwtheswitch.org/white-papers/cmock-intro.html)
 * [Ceedling forum](http://sourceforge.net/p/ceedling/discussion/1052467)
 * [CMock forums](http://sourceforge.net/p/cmock/discussion/822277)
 * [Unity forums](http://sourceforge.net/p/unity/discussion/770030)
