#!/bin/bash
set -eux

if [ "${USE_PYTHON26:-yes}" == "yes" ]; then

    cat > /etc/apt/sources.list.d/deadsnakes.list << EOF
deb http://ppa.launchpad.net/fkrull/deadsnakes/ubuntu precise main
deb-src http://ppa.launchpad.net/fkrull/deadsnakes/ubuntu precise main
EOF

    apt-key add - << EOF
-----BEGIN PGP PUBLIC KEY BLOCK-----
Version: SKS 1.1.4
Comment: Hostname: keyserver.ubuntu.com

mI0ES9WhmQEEAKsEsjsdgYC2nW2oorZlTsi5DhWx4L59Q2QiSyAGNiG+4poKFMWX8o2l9HH/
76TjHJnivOVeEKik1wY+OuqVRYUxvbDv0uscnIDZOpWb+2akcVFrdNWlJ2I7H//DL8eGzFoN
QamM17P3wgnTvZr3B0pTuvFnTd2V1uMmoJeymrnrABEBAAG0HUxhdW5jaHBhZCBPbGQgUHl0
aG9uIFZlcnNpb25ziLYEEwECACAFAkvVoZkCGwMGCwkIBwMCBBUCCAMEFgIDAQIeAQIXgAAK
CRBbuSwJ24JmbGY2BACGPJq16ovctfFSTU3GfQPMzAa97xa5F+WLb94B7gU4qICIyRzTQ6Kl
6N+KSE8Ty+PY/gWz8mxuCMlqWoom3Oc9sFsw3PD1yo+SLxjvtPSIdNvwvAPmzWi2S0xCn5fr
93jznD2X+a5cfqAO76QGYhLBbz77pcBBzEJfDWVXaFbNsw==
=HITP
-----END PGP PUBLIC KEY BLOCK-----
EOF

    apt-get update

    apt-get -qy install python2.6-dev python-distribute-deadsnakes

    easy_install-2.6 virtualenv
else
    apt-get update
    apt-get -qy install python-dev python-virtualenv
fi

# Install other dependences
apt-get -qy install libxen-dev make
