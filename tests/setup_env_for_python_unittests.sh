#!/bin/bash
set -eux

SMROOT=$(cd $(dirname $0) && cd .. && pwd)
ENVDIR="$SMROOT/.env"

if [ "${USE_PYTHON26:-yes}" == "yes" ]; then
    virtualenv-2.6 --no-site-packages "$ENVDIR"
else
    virtualenv "$ENVDIR"
fi

set +u
. "$ENVDIR/bin/activate"
set -u

pip install nose
pip install coverage
pip install xenapi
pip install mock
