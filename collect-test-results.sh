#!/bin/bash

OUTPUT_DIR=$1

if [ -z $OUTPUT_DIR ]; then
    echo "Usage: $0 <output directory>"
    exit 1
fi

mkdir -p $OUTPUT_DIR

lcov --capture --directory . --rc lcov_branch_coverage=1 --no-external --output-file $OUTPUT_DIR/coverage.info
genhtml $OUTPUT_DIR/coverage.info  --rc lcov_branch_coverage=1 --output-directory $OUTPUT_DIR/coverage-html
tar cf $OUTPUT_DIR/test-results.tar `find mockatests -name \*.log`
tar cf $OUTPUT_DIR/gcov-files.tar `find . -name \*.gcda -or -name \*.gcno`
