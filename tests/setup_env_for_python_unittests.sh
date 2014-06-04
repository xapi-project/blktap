#!/bin/bash
set -eux

SMROOT=$(cd $(dirname $0) && cd .. && pwd)
ENVDIR="$SMROOT/.env"

if [ "${USE_PYTHON24:-yes}" == "yes" ]; then
    virtualenv-2.4 --no-site-packages "$ENVDIR"
else
    virtualenv "$ENVDIR"
fi

set +u
. "$ENVDIR/bin/activate"
set -u

if [ "${USE_PYTHON24:-yes}" == "yes" ]; then
    pip install nose==1.2.1
else
    pip install nose
fi

pip install coverage
pip install xenapi
pip install mock
