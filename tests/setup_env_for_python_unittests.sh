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
pip install xenapi
pip install mock

# build xslib.py
# I need -fPIC otherwise I get "relocation R_X86_64_32 against" type errors
PYTHONLIBS=$(dirname $(find /usr/include/ -maxdepth 2 -path \*/python\*/Python.h -type f | head -1))
make -C "$SMROOT/snapwatchd" CFLAGS="-O2 -I${PYTHONLIBS}/ -I/usr/include -shared -fPIC"
