#!/bin/bash
## Set of SM tests for verifying iSCSI attach/detach refcounting

## Source the general system function file
. ./XE_api_library.sh

## source performance tests
. ./performance_functions.sh

# XXX The free and in use lists do not have to be stored in files. However,
# storing them in a file eases post-mortem debugging.
LOGFILE="/tmp/`date +%s-%N`"
FREEFILE="${LOGFILE}-Freelist"
INUSEFILE="${LOGFILE}-Usedlist"

# Initializes the free list (writes one number per line in a file).
init_freelist() 
{
    # FIXME no need for a "for" loop, just do "seq 0 9 > ${FREEFILE}".
    for i in `seq 0 9` ; do
        echo ${i} >> ${FREEFILE}
    done
    touch ${INUSEFILE}
    echo "LOGFILES: ${FREEFILE} and ${INUSEFILE}"
}

# Deletes the free list and in use list files.
cleanup_freelist()
{
    rm ${INUSEFILE} ${FREEFILE}
}


# Returns a random free LUN. The LUN is neither removed from the free list nor
# added to the used list.
# Arguments:
# 1: the size of the free list
# (TODO Couldn't we get the size of the free list by doing "wc -l ${FREEFILE}"?)
getFreeLUN()
{
    rnd=0
    while [ $rnd -eq 0 ]; do
        rnd=$RANDOM
        let "rnd %= ${1}+1"
        VAL=`awk "NR==$rnd" ${FREEFILE}`
        if [ -z $VAL ]; then
            rnd=0
        fi
    done
    echo $VAL
}

# Returns a random SR that is currently using a LUN. The SR is not removed from
# the used list nor added to the free list.
# Arguments:
# 1: size of the in use list
# (TODO Couldn't we get the size of the free list by doing "wc -l ${INUSEFILE}"?)
getUsedSR()
{
    rnd=0
    while [ $rnd -eq 0 ]; do
        rnd=$RANDOM
        let "rnd %= ${1}+1"
        VAL=`awk "NR==$rnd" ${INUSEFILE} | cut -d" " -f2`
        if [ -z $VAL ]; then
            rnd=0
        fi
    done
    echo $VAL
}

# Removes a "LUN SR" mapping from the in use list and returns the LUN to the
# free list.
# Arguments:
# 1: the SR
del_from_usedlist()
{
    SR_ID=${1}
    LUN=`get_LUN_fromSRID ${SR_ID}`

    # Remove the line that contains the specified SR.
    # FIXME replace with "grep -Ev '...' > ${INUSEFILE}"
    line=`grep -n ${1} ${INUSEFILE} | cut -d: -f1`
    awk "NR!=$line" ${INUSEFILE} > ${INUSEFILE}-tmp
    mv ${INUSEFILE}-tmp ${INUSEFILE}

    echo $LUN >> ${FREEFILE}
}

# Allocates the specified LUN by removing it from the free list and adding it to
# he used list.
# Args: LUNid SR_ID
# Arguments:
# 1: the LUN to allocate
# 2: the SR that will be using the LUN
del_from_freelist()
{
    LUN=${1}
    SRID=${2}

    # Remove the line that contains the specified LUN.
    line=`grep -n ^${LUN} ${FREEFILE} | cut -d: -f1`
    awk "NR!=$line" ${FREEFILE} > ${FREEFILE}-tmp
    mv ${FREEFILE}-tmp ${FREEFILE}

    # Write to the in use file that the specified LUN is in use by the specified
    # SR.
    echo $LUN $SRID >> ${INUSEFILE}
}

# Retrieves the SR using the specified LUN.
# Args: LUNid
get_SRID_fromLUN()
{
    #echo "Calling: awk '/^${1}/' ${INUSEFILE}"
    grep ^${1} ${INUSEFILE} | cut -d' ' -f 2
}

# Retrieves the LUN the specified SR is using.
# Arguments:
# 1: The SR.
get_LUN_fromSRID()
{
    #echo "Calling: awk '/^${1}/' ${INUSEFILE}"
    grep ${1} ${INUSEFILE} | cut -d' ' -f 1
}

# Retrives the SCSI ID of the specified LUN.
# Args: LUNid
get_SCSIid_fromLUN()
{
    subject="Querying SCSIid for LUN${1}"
    debug_test "$subject"
    ADDR=`echo ${ISCSI_RECID} | cut -d',' -f1`
    # XXX /etc/xensource/SCSIutil.smrt does not exist in a fresh XS
    # installation, how is it created?
    cmd="$REM_CMD python /etc/xensource/SCSIutil.smrt /dev/iscsi/${LISCSI_TARGET_ID}/${ADDR}/LUN${1}"
    run $cmd
    if [ $RUN_RC -ne 0 ]; then
        debug_result 1
        incr_exitval
    else
        debug_result 0
    fi
    test_exit 1
    GLOBAL_RET=$RUN_OUTPUT
}

# Initializes the SCSIidcache array with the IDs of the iSCSI LUNs.
# Arguments: none.
init_SCSIid_list()
{
    GLOBAL_DEBUG=0
    verify_device
    ret=$?
    GLOBAL_DEBUG=1

    # Initialize the LUNs if not already done so so we can get their IDs. Undo
    # this operation before returning from this function (by setting the RESET
    # flag).
    if [ $ret -gt 0 ]; then
        RESET=1
        iqn_initialise
        open_iscsi_start
        discover_target
        if [ $? -gt 0 ]; then
            debug "discover_target failed"
            test_exit 1
        fi

        attach_target ${ISCSI_RECID} ${ISCSI_RECIQN}
        # Wait for devices to appear
        sleep 5
    else
        RESET=0
    fi
    test_exit 0

    verify_LUNcount
    for i in `seq 0 9`; do
        get_SCSIid_fromLUN ${i}
        SCSIidcache[$i]=$GLOBAL_RET
        debug "Retrieved SCSIid $GLOBAL_RET for LUN $i"
    done

    if [ $RESET == 1 ]; then
        detach_target ${ISCSI_RECID} ${ISCSI_RECIQN}
    fi
}

# Initializes the SR on the specified LUN.
# Arguments:
# 1: the LUN to use
setup_sr()
{
    SCSIid=${SCSIidcache[${1}]}
    smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
        "NULL" "NULL" "${IQN_INITIATOR_ID}" "${LISCSI_TARGET_IP}" \
        "${LISCSI_TARGET_ID}" "${SCSIid}"
}

# FIXME This doesn't seem to work.
discover_LUNs()
{
    # Call sr_create with no LUNid arg
    GLOBAL_DEBUG=0
    setup_sr
    test_exit 0
    GLOBAL_DEBUG=1
}

# Tells whether the iSCSI target exists under /dev.
# Arguments: none.
testDevPath()
{
    cmd="$REM_CMD test -e /dev/iscsi/${LISCSI_TARGET_ID}"
    run $cmd
    GLOBAL_RET=$RUN_RC
}

# Ensures that all 10 LUNs are present.
# Arguments: none.
verify_LUNcount()
{
    RC=0
    for i in `seq 0 9`; do
        DEVPATH="/dev/iscsi/${LISCSI_TARGET_ID}/LUN${i}"
        cmd="$REM_CMD test -e ${DEVPATH}"
        run $cmd
        RC=`expr ${RC} + ${RUN_RC}`
    done
    if [ $RC -ne 0 ]; then
        debug "Not all LUNs present, this test requires 10 LUNs."
        debug "Unable to continue tests, exiting quietly."
        cleanup_SRs
        cleanup_freelist
        exit 0
    fi
}

# Tells whether the iSCSI reference path exists.
# Arguments: none.
# TODO What's an iSCSI reference path?
testIscsiRefPath()
{
    cmd="$REM_CMD test -e /var/run/sr-ref/${LISCSI_TARGET_ID}"
    run $cmd
    GLOBAL_RET=$RUN_RC
}

# Prints the number of references.
# Arguments: none.
# FIXME incomplete comment
getRefCount()
{
    testIscsiRefPath
    if [ $GLOBAL_RET -ne 0 ]; then
        echo 0
        return
    fi

    cmd="$REM_CMD wc -l /var/run/sr-ref/${LISCSI_TARGET_ID}"
    run $cmd
    VAL=`echo ${RUN_OUTPUT}  | cut -d" " -f1`
    echo $VAL
}

# XXX incomplete
# Args: SRid
debug_SRDelete()
{
    # Debug disabled
    return
    debug "Deleting SR [$1]"
    LUN=`get_LUN_fromSRID ${1}`
    debug "     LUN: $LUN"
    debug "     SCSIid: ${SCSIidcache[${LUN}]}"
}

# Destroys all SRs by checking the in use list.
# Arguments: none.
cleanup_SRs()
{
    debug "Cleaning up the SRs"
    USEDSIZE=`wc -l ${INUSEFILE} | cut -d" " -f1`
    if [ $USEDSIZE == 0 ]; then
        return
    fi
    for i in `seq 1 ${USEDSIZE}`; do
        UsedSR=`getUsedSR $USEDSIZE`
        SR_unplug ${UsedSR}
        debug_SRDelete ${UsedSR}
        sm_SRDelete ${UsedSR}
        test_exit 1
        del_from_usedlist ${UsedSR}
    done
}

# Performs a verification test: create some SRs and check the LUN reference
# counter.
run_manual_verify_test()
{
    debug ""
    debug "Running manual verification tests"
    debug "================================="
    debug ""

    debug && debug "TEST 1: Add SR, verify refcount"
    setup_sr 0
    test_exit 1
    del_from_freelist 0 ${SR_ID}
    echo "Created SR [$SR_ID]"
    debug_test "Verify refcount for LUN 0"
    testDevPath
    if [ $GLOBAL_RET -ne 0 ]; then
        debug_result 1
        return 1
    fi
    REF=`getRefCount`
    if [ $REF -ne 1 ]; then
        debug_result 1
        return 1
    fi
    debug_result 0

    verify_LUNcount

    SR_unplug ${SR_ID}
    debug_test "Verify refcount for LUN 0"
    testDevPath
    if [ $GLOBAL_RET -eq 0 ]; then
        debug_result 1
        return 1
    fi
    REF=`getRefCount`
    if [ $REF -ne 0 ]; then
        debug_result 1
        return 1
    fi
    debug_result 0

    #discover_LUNs
    debug_test "Discover LUN verify refcount"
    testDevPath
    if [ $GLOBAL_RET -eq 0 ]; then
        debug_result 1
        return 1
    fi
    REF=`getRefCount`
    if [ $REF -ne 0 ]; then
        debug_result 1
        return 1
    fi
    debug_result 0

    SR_ID=`get_SRID_fromLUN 0`
    debug_SRDelete ${SR_ID}
    sm_SRDelete ${SR_ID}
    del_from_usedlist ${SR_ID}

    debug && debug "TEST 2: Add 2 SRs, verify refcount, unplug one and reverify"
    for i in `seq 0 1`; do
        setup_sr $i
        test_exit 1
        del_from_freelist $i ${SR_ID}
    done

    debug_test "Verify refcount for LUNs 0 and 1"
    testDevPath
    if [ $GLOBAL_RET -ne 0 ]; then
        debug_result 1
        return 1
    fi
    REF=`getRefCount`
    if [ $REF -ne 2 ]; then
        debug_result 1
        return 1
    fi
    debug_result 0

    SR_ID=`get_SRID_fromLUN 1`
    SR_unplug ${SR_ID}
    debug_test "Verify refcount for LUN 0"
    testDevPath
    if [ $GLOBAL_RET -ne 0 ]; then
        debug_result 1
        return 1
    fi
    REF=`getRefCount`
    if [ $REF -ne 1 ]; then
        debug_result 1
        return 1
    fi
    debug_result 0

    #discover_LUNs
    debug_test "Discover LUN verify refcount"
    testDevPath
    if [ $GLOBAL_RET -ne 0 ]; then
        debug_result 1
        return 1
    fi
    REF=`getRefCount`
    if [ $REF -ne 1 ]; then
        debug_result 1
        return 1
    fi
    debug_result 0

    SR_ID=`get_SRID_fromLUN 0`
    SR_unplug ${SR_ID}
    for i in `seq 0 1`; do
        SR_ID=`get_SRID_fromLUN $i`
        debug_SRDelete  ${SR_ID}
        sm_SRDelete ${SR_ID}
        del_from_usedlist ${SR_ID}
    done

    debug && debug "TEST 3: Add 10 SRs, verify refcount, unplug 9 and reverify"
    for i in `seq 0 9`; do
        setup_sr $i
        test_exit 1
        del_from_freelist $i ${SR_ID}
    done
    debug_test "Verify refcount for LUNs 0 - 9"
    testDevPath
    if [ $GLOBAL_RET -ne 0 ]; then
        debug_result 1
        return 1
    fi
    REF=`getRefCount`
    if [ $REF -ne 10 ]; then
        debug_result 1
        return 1
    fi
    debug_result 0


    REFCOUNT=9
    for i in `seq 0 9`; do
        if [ $i == 6 ]; then
            continue
        fi
        SR_ID=`get_SRID_fromLUN $i`
        SR_unplug ${SR_ID}
        debug_test "Verify refcount for ${REFCOUNT} LUNs"
        testDevPath
        if [ $GLOBAL_RET -ne 0 ]; then
            debug_result 1
            return 1
        fi
        REF=`getRefCount`
        if [ $REF -ne $REFCOUNT ]; then
            debug_result 1
            return 1
        fi
        debug_result 0
        REFCOUNT=`expr ${REFCOUNT} - 1`
    done

    debug_test "Verify refcount for single LUN"
    testDevPath
    if [ $GLOBAL_RET -ne 0 ]; then
        debug_result 1
        return 1
    fi
    REF=`getRefCount`
    if [ $REF -ne 1 ]; then
        debug_result 1
        return 1
    fi
    debug_result 0

    SR_ID=`get_SRID_fromLUN 6`
    SR_unplug ${SR_ID}
    for i in `seq 0 9`; do
        SR_ID=`get_SRID_fromLUN $i`
        debug_SRDelete ${SR_ID}
        sm_SRDelete ${SR_ID}
        del_from_usedlist ${SR_ID}
    done

    return 0
}

# Performs the probability test: randomly create and destroy SRs.
run_probability_test()
{
    debug ""
    debug "Running probability test"
    debug "========================"
    debug ""


    MAXSEQ=10
    if [ ! -z ${FAST} ]; then
        MAXLOOP=1
    else
        MAXLOOP=100
    fi
    loop=0
    TEST=1

    # Repeat MAXLOOP times. In the 1st iteration TEST is 1 so the inner "if"
    # statement will be executed with 90% probability (x10). In the 2nd
    # iteration TEST will be 6 so the "if" will be executed with 40%
    # probability. From the 3rd iteration and on, TEST is 1 so the "if" will be
    # executed with 80% probability.
    while [ $loop -lt $MAXLOOP ]; do
        # repeat for all LUNs
        for i in `seq 1 $MAXSEQ` ;  do

            # pick a random number from 0 to 9
            let "NUM = $RANDOM % 9"

            # if NUM > TEST, create an SR on a random LUN
            if [ $NUM -gt $TEST ]; then
                FREESIZE=`wc -l ${FREEFILE} | cut -d" " -f1`
                if [ $FREESIZE == 0 ]; then
                    continue
                fi
                FreeLUN=`getFreeLUN $FREESIZE`
                setup_sr ${FreeLUN}
                test_exit 1
                del_from_freelist ${FreeLUN} ${SR_ID}
            # if NUM <= TEST, randomly destroy an SR
            else
                USEDSIZE=`wc -l ${INUSEFILE} | cut -d" " -f1`
                if [ $USEDSIZE == 0 ]; then
                    continue
                fi
                UsedSR=`getUsedSR $USEDSIZE`
                SR_unplug ${UsedSR}
                debug_SRDelete  ${UsedSR}
                sm_SRDelete ${UsedSR}
                test_exit 1
                del_from_usedlist ${UsedSR}
            fi
            #discover_LUNs
        done

        # Verify refcount matches usedlist
        debug_test "Verifying refcount"
        REF=`getRefCount`
        USEDSIZE=`wc -l ${INUSEFILE} | cut -d" " -f1`
        if [ $REF -ne ${USEDSIZE} ]; then
            debug_result 1
            return 1
        fi
        debug_result 0

        # Flip probability
        if [ $TEST == 0 ]; then
            TEST=6
        else
            TEST=1
        fi
        loop=`expr $loop + 1`
    done
}


run_tests()
{
    gen_hostlist

    DRIVER_TYPE=lvmoiscsi
    SUBSTRATE_TYPE=lvmoiscsi
    CONTENT_TYPE=user

    init_freelist

    init_SCSIid_list

    run_manual_verify_test
    test_exit 1

    cleanup_SRs

    run_probability_test

    cleanup_SRs

    cleanup_freelist

    debug_test "Verifying refcount post stress loop"
    testDevPath
    if [ $GLOBAL_RET -eq 0 ]; then
        debug_result 1
        return 1
    fi
    REF=`getRefCount`
    if [ $REF -ne 0 ]; then
        debug_result 1
        return 1
    fi
    debug_result 0
}

TEMPLATE_ALIAS=windows


process_arguments $@

post_process

check_req_args

check_req_sw

install_ssh_certificate ${REMHOSTNAME} ${SSH_PRIVATE_KEY} ${PASSWD}

install_scsiID_helper ${REMHOSTNAME}

print_version_info

if [[ -z ${IQN_INITIATOR_ID} || -z ${LISCSI_TARGET_IP} || -z ${LISCSI_TARGET_ID} ]]; then
    debug "iSCSI configuration information missing. Skipping test"
    exit
fi

if [ ! -z ${IQN_INITIATOR_ID_CHAP} ] ; then
    debug "Not ready to run these tests with CHAP credentials, Exiting quietly."
    exit
fi
run_tests

print_exit_info

