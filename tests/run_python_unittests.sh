#!/bin/bash
set -eux

SMROOT=$(cd $(dirname $0) && cd .. && pwd)
ENVDIR="$SMROOT/.env"

set +u
. "$ENVDIR/bin/activate"
set -u

(
    cd "$SMROOT"
    PYTHONPATH="$SMROOT/snapwatchd:$SMROOT/drivers/" \
        coverage run $(which nosetests) \
            --with-xunit \
            --xunit-file=nosetests.xml \
            tests
    coverage xml --include "$SMROOT/drivers/*"
)
