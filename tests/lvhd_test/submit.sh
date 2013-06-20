#!/bin/bash
#
# Submits the unit tests for execution.

function main()
{
    if [ -z ${1} ]; then
        declare -r build="http://10.219.192.30/root/usr/groups/xen/carbon/trunk-storage/54221"
        echo "using default build ${build}"

    #declare -r build="http://10.219.192.30/root/usr/groups/xen/carbon/sanibel/53853"
    else
        declare -r build=${1}
    fi

    # Check that the build exists.
    wget "${build}" -O - > /dev/null 2>&1
    rc=$?
    if [ ${rc} -ne 0 ]; then
        echo "build ${build} does not exist"
        exit $?
    fi

    xenrt submit \
        --testcasefiles smunittests.py \
        --customsequence smunittests.seq \
        --pool VMX,SVM,48NODE \
        -D INPUTDIR="${build}"
}

main "$@"
