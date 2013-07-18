#!/bin/bash
set -eux

SMROOT=$(cd $(dirname $0) && cd .. && pwd)
ENVDIR="$SMROOT/.env"

set +u
. "$ENVDIR/bin/activate"
set -u

(
    cd "$SMROOT"
    PYTHONPATH="$SMROOT/snapwatchd:$SMROOT/drivers/" nosetests tests/test_ISCSISR.py
)
