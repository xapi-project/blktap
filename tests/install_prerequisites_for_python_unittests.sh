#!/bin/bash
set -eux

if [ "${USE_PYTHON24:-yes}" == "yes" ]; then
    apt-get -qy install python-software-properties

    apt-add-repository ppa:fkrull/deadsnakes -y

    apt-get update
    
    apt-get -qy install python2.4-dev python-distribute-deadsnakes

    easy_install-2.4 virtualenv==1.7.2
else
    apt-get update
    apt-get -qy install python-dev python-virtualenv
fi

# Install other dependences
apt-get -qy install swig libxen-dev make
