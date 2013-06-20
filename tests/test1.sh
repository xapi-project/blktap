#!/bin/bash
## Set of SM tests for default SR

## Source the general system function file
. ./XE_api_library.sh

## source performance tests
. ./performance_functions.sh

DD_HAS_RUN=0
BONNIE_HAS_RUN=0

# Performs some basic validation tests: creates an SR, adds a VDI, retrieves
# the SR and VDI parameters, deletes the VDI, deletes the SR.
# TODO add resize, clone, snapshot
run_basic_sr_validation_tests () {
    debug "TG: Basic SR Validation Tests"

    # Create the SR.
    smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
        "${NFSSERVER}" "${NFSSERVERPATH}" \
        "${USE_IQN_INITIATOR_ID}" "${LISCSI_TARGET_IP}" "${LISCSI_TARGET_ID}" \
        "${LISCSI_ISCSI_ID}" "${NAPP_TARGET}" "${NAPP_USER}" "${NAPP_PASSWD}" \
	"${NAPP_AGGR}" "${NAPP_FVOLS}" "${NAPP_SIZE}" "${EQL_TARGET}" \
	"${EQL_USER}" "${EQL_PASSWD}" "${EQL_SPOOL}"
    test_exit 1

    # Add the VDI.
    smAddVdi ${SR_ID} ${DEVSTRING} "TestVDI" ${VDI_SIZE}
    test_exit 1

    smGetParams ${SR_ID} test_exit 1

    smVdiGetParams ${SR_ID} ${VDI_ID}
    test_exit 1

    smDeleteVdi ${SR_ID} ${DEVSTRING} ${VDI_ID}
    test_exit 1

    smDelete ${SR_ID} 
    test_exit 1
}

# Performs some more advanced validation tests. If "oldlvm" is specified as the
# first argument, it creates an SR, adds a VDI, retrieves the VDI parameters,
# resizes the VDI, deletes the VDI, deletes the SR. Independently of whether
# "oldlvm" has been supplied, it creates an SR, sets it as the default one,
# installs a VM, clones it, boots it, stops it, uninstalls it (along with the
# cloned one), deletes the SR, and restores the orinigal one as the default
# one. 
# Arguments:
# TODO document arguments
run_advanced_sr_validation_tests () {
    debug "TG: Advanced SR Validation Tests"

    ##### Test VDI Resize
    #if [[ $1 = "lvm" || $1 = "lvmoiscsi" ]] ; then
    if [[ $1 = "oldlvm" ]] ; then
        # Create a new SR for our testing
	smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
            "${NFSSERVER}" "${NFSSERVERPATH}" \
            "${USE_IQN_INITIATOR_ID}" "${LISCSI_TARGET_IP}" "${LISCSI_TARGET_ID}" \
            "${LISCSI_ISCSI_ID}" "${NAPP_TARGET}" "${NAPP_USER}" "${NAPP_PASSWD}" \
	    "${NAPP_AGGR}" "${NAPP_FVOLS}" "${NAPP_SIZE}" "${EQL_TARGET}" \
	    "${EQL_USER}" "${EQL_PASSWD}" "${EQL_SPOOL}"
        test_exit 1
    
        # Create a VDI
        VDI_SZ=$((5*1024*1024*1024))
        smAddVdi ${SR_ID} ${DEVSTRING} "TestVDI" ${VDI_SZ}
        test_exit 1
        debug "VDI ID ${VDI_ID}"
    
        smVdiGetParams ${SR_ID} ${VDI_ID}
        test_exit 1
    
        # Resize the VDI
        VDI_SZ=$((10*1024*1024*1024))
        smResizeVdi ${SR_ID} ${VDI_ID} ${VDI_SZ}
        test_exit 1
    
        # Delete the VDI
        smDeleteVdi ${SR_ID} ${DEVSTRING} ${VDI_ID}
        test_exit 1
    
        # Delete the SR
        smDelete ${SR_ID}
        test_exit 1
    fi

    ### Test Cloning of VMs
    # Create a new SR for our testing
    smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
        "${NFSSERVER}" "${NFSSERVERPATH}" \
        "${USE_IQN_INITIATOR_ID}" "${LISCSI_TARGET_IP}" "${LISCSI_TARGET_ID}" \
        "${LISCSI_ISCSI_ID}" "${NAPP_TARGET}" "${NAPP_USER}" "${NAPP_PASSWD}" \
	"${NAPP_AGGR}" "${NAPP_FVOLS}" "${NAPP_SIZE}" "${EQL_TARGET}" \
	"${EQL_USER}" "${EQL_PASSWD}" "${EQL_SPOOL}"
    test_exit 1

    #Store away the old default SR_ID and switch the default SR to the new SR
    smGetDefaultSR
    local OLD_SR_ID=$SR_DEFAULT_ID
    smSetDefaultSR ${SR_ID}

    # Install the source guest domain
    NAME="testVM.$DATE"
    pinstall_VM ${NAME} 256 $SR_ID
    test_exit 1

    VM_ID=${GLOBAL_RET}
    debug "VM ID for ${NAME}: $VM_ID"

    # CA-6682: Commenting out some testing as a result of networking issue
    # Start up the source VM
    # smBootVM ${VM_ID}
    # test_exit 1

    # getVMIP ${VM_ID} 0
    # test_exit 1
    # VM_IP=${GLOBAL_RET}

    # Confirm that the source VM is accessible
    # CARBON_DOMU_PASSWORD="xensource"
    # install_ssh_certificate ${VM_IP} ${SSH_PRIVATE_KEY} ${CARBON_DOMU_PASSWORD}

    # Stop the source VM
    # smStopVM ${VM_ID}
    # test_exit 1

    # Clone the VM
    CLONE_NAME="Clone of ${NAME}"
    cloneVM ${VM_ID} "${CLONE_NAME}"
    CLONE_VM_ID=${GLOBAL_RET}
    debug "VM ID for ${CLONE_NAME}: $CLONE_VM_ID"
    test_exit 1

    # Start up the cloned VM
    smBootVM ${CLONE_VM_ID}
    test_exit 1

    getVMIP ${CLONE_VM_ID} 0
    test_exit 1
    CLONE_VM_IP=${GLOBAL_RET}

    # Confirm that the cloned VM is accessible
    CARBON_DOMU_PASSWORD="xensource"
    install_ssh_certificate ${CLONE_VM_IP} ${SSH_PRIVATE_KEY} ${CARBON_DOMU_PASSWORD}

    # Stop the cloned VM
    smStopVM ${CLONE_VM_ID}
    test_exit 1

    # Uninstall both VMs
    smUninstallVM ${SR_ID} ${CLONE_VM_ID}
    test_exit 1
    smUninstallVM ${SR_ID} ${VM_ID}
    test_exit 1

    # Reset the default SR
    smSetDefaultSR ${OLD_SR_ID}
    test_exit 1

    # Delete the SR
    smDelete ${SR_ID}
    test_exit 1
}

# Replugs a VBD.
replug_vbd() {
    local VBD_ID=$1

    smUnplugVbd ${VBD_ID}
    test_exit 1
    smPlugVbd ${VBD_ID}
    test_exit 1

    sleep 1
}

# Performs a performance test on dom0: it creates an SR, adds a VDI, performs
# some dd tests on it, destroys the VDI & the SR. The performance results are
# just printed, the function does not check if they're good enough.
run_dom0_performance_tests() {
    DEVICE="xvda"
    DEV_NAME="/dev/${DEVICE}"
    BS=4K
    if [ -n "$FAST" ] ; then
        COUNT=10K
    else
        COUNT=500K
    fi

    debug "TG: SR Dom0 Performance Tests"
    smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
        "${NFSSERVER}" "${NFSSERVERPATH}" \
        "${USE_IQN_INITIATOR_ID}" "${LISCSI_TARGET_IP}" "${LISCSI_TARGET_ID}" \
        "${LISCSI_ISCSI_ID}" "${NAPP_TARGET}" "${NAPP_USER}" "${NAPP_PASSWD}" \
	"${NAPP_AGGR}" "${NAPP_FVOLS}" "${NAPP_SIZE}" "${EQL_TARGET}" \
	"${EQL_USER}" "${EQL_PASSWD}" "${EQL_SPOOL}"
    test_exit 1

    smAddVdi ${SR_ID} ${DEVSTRING} "TestVDI" ${VDI_SIZE}
    test_exit 1


    smCreateVbd ${VDI_ID} ${DOM0_ID} ${DEVICE} 
    test_exit 1

    smPlugVbd ${VBD_ID}
    test_exit 1

    #################################
    ## dom0 tests:
    dd_test "$REM_CMD" ${DEV_NAME} /dev/null ${BS} ${COUNT}
    dom0_read=$DD_RESULT

    ## destroy buffer cache
    replug_vbd ${VBD_ID}

    dd_test "$REM_CMD" /dev/zero ${DEV_NAME} ${BS} ${COUNT}
    dom0_write=$DD_RESULT

    replug_vbd ${VBD_ID}

    dd_test "$REM_CMD" ${DEV_NAME} /dev/null ${BS} ${COUNT}
    dom0_reread=$DD_RESULT

    replug_vbd ${VBD_ID}

    dd_test "$REM_CMD" /dev/zero ${DEV_NAME} ${BS} ${COUNT}
    dom0_rewrite=$DD_RESULT

    debug "dom0_read/write / reread/rewrite (BS=${BS}, count=${COUNT}):
${dom0_read}/${dom0_write} / ${dom0_reread}/${dom0_rewrite}"
    #################################

    smUnplugVbd ${VBD_ID}
    test_exit 1

    smDeleteVbd ${VBD_ID}
    test_exit 1

    smDeleteVdi ${SR_ID} ${DEVSTRING} ${VDI_ID}
    test_exit 1

    smDelete ${SR_ID}
    test_exit 0
}

# Performs a storage stress test on dom0: creates an SR, adds & attaches a VDI,
# runs a data integrity test, deletes the VDI & the SR.
# TODO document arguments
run_dom0_stress_tests() {
    DEVICE="xvda"
    DEV_NAME="/dev/${DEVICE}"
    if [ -n "$FAST" ] ; then
        NUM_ITER=25000
    else
        NUM_ITER=500000
    fi

    debug "TG: SR Dom0 Stress Tests"

    smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
        "${NFSSERVER}" "${NFSSERVERPATH}" \
        "${USE_IQN_INITIATOR_ID}" "${LISCSI_TARGET_IP}" "${LISCSI_TARGET_ID}" \
        "${LISCSI_ISCSI_ID}" "${NAPP_TARGET}" "${NAPP_USER}" "${NAPP_PASSWD}" \
	"${NAPP_AGGR}" "${NAPP_FVOLS}" "${NAPP_SIZE}" "${EQL_TARGET}" \
	"${EQL_USER}" "${EQL_PASSWD}" "${EQL_SPOOL}"
    test_exit 1

    smAddVdi ${SR_ID} ${DEVSTRING} "TestVDI" ${VDI_SIZE}
    test_exit 1


    smCreateVbd ${VDI_ID} ${DOM0_ID} ${DEVICE} 
    test_exit 1

    smPlugVbd ${VBD_ID}
    test_exit 1

    run_fsx "$REM_CMD" ${DEV_NAME}  ${NUM_ITER}
    test_exit 1

    smUnplugVbd ${VBD_ID}
    test_exit 1

    smDeleteVbd ${VBD_ID}
    test_exit 1

    smDeleteVdi ${SR_ID} ${DEVSTRING} ${VDI_ID}
    test_exit 1

    smDelete ${SR_ID}
    test_exit 0
}

# Performs specific performance tests on a domU: creates an SR, installs a VM
# on it & boots it, creates and attaches a VDI to the VM, executes dd or bonnie
# on that VDI, deletes the VDI, the VM, and the SR. 
# TODO document args
# Arguments:
# 1: dd or bonnie
# $1 is type of test
run_domU_specific_performance_test() {
    DEVICE="xvdc"
    DEV_NAME="/dev/${DEVICE}"
    BS=4K
    if [ -n "$FAST" ] ; then
        COUNT=10K
    else
        COUNT=500K
    fi

    CARBON_DOMU_PASSWORD="xensource"

    if [ -z $1 ] ; then
        debug "Unknown test: Pass in 'dd' or 'bonnie'"
        return 1
    fi

    if [ ! $1 = "bonnie" -a ! $1 = "dd" ] ; then 
        debug "Unknown test: Pass in 'dd' or 'bonnie'"
        return 1
    fi

    TEST_TYPE=$1

    if [ ${TEST_TYPE} = "dd" ] ; then
        debug "TG: dd performance"
    elif [ ${TEST_TYPE} = "bonnie" ] ; then
        debug "TG: bonnie++ performance"    
    fi

    smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
        "${NFSSERVER}" "${NFSSERVERPATH}" \
        "${USE_IQN_INITIATOR_ID}" "${LISCSI_TARGET_IP}" "${LISCSI_TARGET_ID}" \
        "${LISCSI_ISCSI_ID}" "${NAPP_TARGET}" "${NAPP_USER}" "${NAPP_PASSWD}" \
	"${NAPP_AGGR}" "${NAPP_FVOLS}" "${NAPP_SIZE}" "${EQL_TARGET}" \
	"${EQL_USER}" "${EQL_PASSWD}" "${EQL_SPOOL}"
    test_exit 1

    #Store the old default SR_ID
    smGetDefaultSR
    test_exit 1

    local OLD_SR_ID=$SR_DEFAULT_ID

    smSetDefaultSR ${SR_ID}
    test_exit 1

    # start guest domain
    NAME="perfVM.$DATE"
    pinstall_VM ${NAME} 256 $SR_ID

    VM_ID=${GLOBAL_RET}
    test_exit 1

    smBootVM ${VM_ID}
    test_exit 1

    getVMIP ${VM_ID} 0
    DOMU_IP=${GLOBAL_RET}

    install_ssh_certificate ${DOMU_IP} ${SSH_PRIVATE_KEY} ${CARBON_DOMU_PASSWORD}

    smAddVdi ${SR_ID} ${DEVSTRING} "TestVDI" ${VDI_SIZE}
    test_exit 1

    smCreateVbd ${VDI_ID} ${VM_ID} ${DEVICE} 
    test_exit 1

    smPlugVbd ${VBD_ID}
    test_exit 1

    if [ ${TEST_TYPE} = "dd" ] ; then
        DD_HAS_RUN=1
        DOMU_REM_CMD="ssh -q -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null -i ${SSH_PRIVATE_KEY} root@${DOMU_IP}"
        dd_test "$DOMU_REM_CMD" ${DEV_NAME} /dev/null ${BS} ${COUNT}
        blkback_read=$DD_RESULT

        ## destroy buffer cache
        replug_vbd ${VBD_ID}

        dd_test "$DOMU_REM_CMD" /dev/zero ${DEV_NAME} ${BS} ${COUNT}
        blkback_write=$DD_RESULT

        replug_vbd ${VBD_ID}

        dd_test "$DOMU_REM_CMD" ${DEV_NAME} /dev/null ${BS} ${COUNT}
        blkback_reread=$DD_RESULT

        replug_vbd ${VBD_ID}

        dd_test "$DOMU_REM_CMD" /dev/zero ${DEV_NAME} ${BS} ${COUNT}
        blkback_rewrite=$DD_RESULT

        debug "domU_read/write / reread/rewrite (BS=${BS}, count=${COUNT}):
${blkback_read}/${blkback_write} / ${blkback_reread}/${blkback_rewrite}"

    elif [ ${TEST_TYPE} = "bonnie" ] ; then
        BONNIE_HAS_RUN=1
        DOMU_REM_CMD="ssh -q -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null -i ${SSH_PRIVATE_KEY} root@${DOMU_IP}"
        MOUNT_POINT="/var/testmount"

        # install bonnie on VM
	xenrt_tailor
        test_exit 1

        bonnie_install
        test_exit 1

        run $DOMU_REM_CMD mkfs -t ext3 ${DEV_NAME} > /dev/null 2>&1
        run $DOMU_REM_CMD mkdir -p $MOUNT_POINT> /dev/null 2>&1
        run $DOMU_REM_CMD mount ${DEV_NAME} $MOUNT_POINT> /dev/null 2>&1
        bonnie_test blkback "$DOMU_REM_CMD" $MOUNT_POINT
        bonnie_blkback="$BONNIE_RESULT"

        test_exit 1

        debug "$bonnie_blkback" > /dev/null
        run $DOMU_REM_CMD umount $MOUNT_POINT> /dev/null 2>&1
        #################################
    fi

    smUnplugVbd ${VBD_ID}
    test_exit 1

    smDeleteVbd ${VBD_ID}
    test_exit 1

    smDeleteVdi ${SR_ID} ${DEVSTRING} ${VDI_ID}
    test_exit 1

    smStopVM ${VM_ID}
    test_exit 1

    smUninstallVM ${SR_ID} ${VM_ID}
    test_exit 1
    
    smSetDefaultSR ${OLD_SR_ID}
    test_exit 0

    smDelete ${SR_ID}
    test_exit 1
}

# TODO I'd assume this is not yet really important.
graph_results() {
    RESULTS_DIR="/usr/groups/xen/blktap_perf/${REMHOSTNAME}"
    WEB_DIR="/usr/groups/xenrt/jobsched/storage/${REMHOSTNAME}"
    
    mkdir -p $RESULTS_DIR
    mkdir -p $WEB_DIR

    if [ $DD_HAS_RUN -eq 1 ] ; then
        ## graph dd results
        plot_reads  $SUBSTRATE_TYPE $dom0_read $dom0_reread $blkback_read $blkback_reread
        plot_writes $SUBSTRATE_TYPE $dom0_write $dom0_rewrite $blkback_write $blkback_rewrite
    fi

    if [ $BONNIE_HAS_RUN -eq 1 ] ; then
        #grap bonnie data
        bonnie_graph_data "$bonnie_dom0" "$bonnie_blkback" 
        test_exit 0
    fi
}

# Performs some performance tests on domU.
run_domU_performance_tests() {
    debug "TG: SR DomU Performance Tests"
    run_domU_specific_performance_test "dd"

# TODO : find out why apt-get update bonnie++ fails in xenrt env
    run_domU_specific_performance_test "bonnie"

    graph_results
}

# Performs some data integrity tests on dom0: it creates an SR & a VDI,
# executes the biotest on it, and then it destroys the VDI & the SR.
run_dom0_data_integrity_tests() {
    debug "TG: Dom0 Data Integrity Tests"                             
    DEVICE="xvda"
    DEV_NAME="/dev/${DEVICE}"

    smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
        "${NFSSERVER}" "${NFSSERVERPATH}" \
        "${USE_IQN_INITIATOR_ID}" "${LISCSI_TARGET_IP}" "${LISCSI_TARGET_ID}" \
        "${LISCSI_ISCSI_ID}" "${NAPP_TARGET}" "${NAPP_USER}" "${NAPP_PASSWD}" \
	"${NAPP_AGGR}" "${NAPP_FVOLS}" "${NAPP_SIZE}" "${EQL_TARGET}" \
	"${EQL_USER}" "${EQL_PASSWD}" "${EQL_SPOOL}"
    test_exit 1

    smAddVdi ${SR_ID} ${DEVSTRING} "TestVDI" ${VDI_SIZE}
    test_exit 1

    smCreateVbd ${VDI_ID} ${DOM0_ID} ${DEVICE} 
    test_exit 1

    smPlugVbd ${VBD_ID}
    test_exit 1
    
    #################################

    if [ -n "$FAST" ] ; then
        DATAMB=128
    else
        DATAMB=1024
    fi

    biotest "$REM_CMD" $DEV_NAME $DATAMB
    test_exit 1
    
    #################################

    smUnplugVbd ${VBD_ID}
    test_exit 1

    smDeleteVbd ${VBD_ID}
    test_exit 1

    smDeleteVdi ${SR_ID} ${DEVSTRING} ${VDI_ID}
    test_exit 1

    smDelete ${SR_ID}
    test_exit 0
}

# Fills a disk with a pattern.
# TODO document args
# Args:
# 1 -> DEVICE
# 2 -> chunksizeKB
# 3 -> iterations
# 4 -> pad bytes
generate_pattern()
{
    debug "Generating pattern on device ${1} with parameters:"
    debug "      Chunksize : $2KB"
    debug "      Iterations: $3"
    debug "      Pad Bytes : $4B"

    subject="Writing disk test pattern"
    debug_test "$subject"

    # TODO What's tp?
    cmd="$REM_CMD /opt/xensource/debug/tp $1 $2 $3 $4"
    run "$cmd"
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi
    debug_result 0
}

# Compares two disks for equality.
# TODO document args
verify_match()
{
    subject="Verifying disk contents match"
    debug_test "$subject"

    cmd="$REM_CMD diff $1 $2 >> /dev/null"
    run "$cmd"
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi
    debug_result 0
}

# Compares two disks for inequality.
# TODO document args
verify_nomatch()
{
    subject="Verifying disk contents do not match"
    debug_test "$subject"

    cmd="$REM_CMD diff $1 $2 >> /dev/null"
    run "$cmd"
    if [ $RUN_RC -eq 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi
    debug_result 0
}

# Fills a disk with zeros.
# Arguments:
# 1: the disk device to zero
zero_disk()
{
    debug_test "Zeroing disk"

    cmd="$REM_CMD /bin/dd if=/dev/zero of=${1} bs=1M count=20 &> /dev/null"
    run "$cmd"
    if [ $RUN_RC -ne 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
    fi
}

# Performs some VDI snapshot tests: creates an SR and performs ten repetitions
# of the following sequence of operations: add two VDIs, write the same test
# pattern on both, snapshots the first one, zeros the second one, snapshots the
# second one, attaches the two snapshots in read-only mode, verifies them for
# equality and inequality (TODO explain in detail), respectively, and finally
# destroys all the VDIs, all the snapsots, and the SR.
run_vdi_snapshot_tests() {
    if [[ ${SUBSTRATE_TYPE} = "netapp" ]] ; then
        debug "TG: Netapp/EqualLogic SR Snapshot Tests, for verify vmhint and epochhint functionality"                             
    else
         debug "TG: EqualLogic SR Snapshot Tests"
    fi
    DEVICE1="xvda"
    DEVICE2="xvdb"
    DEVICE3="xvdc"
    DEVICE4="xvdd"
    DEV_NAME1="/dev/${DEVICE1}"
    DEV_NAME2="/dev/${DEVICE2}"
    DEV_NAME3="/dev/${DEVICE3}"
    DEV_NAME4="/dev/${DEVICE4}"
    # 20MB disks
    VSIZE=$((20*1024*1024))
    if [[ ${SUBSTRATE_TYPE} = "netapp" ]] ; then
        VMHINT=`uuidgen`
    fi
    LOOP=10

    cmd="$REM_CMD test -f /opt/xensource/debug/tp"
    run "$cmd"
    if [ $RUN_RC -gt 0 ]; then
        debug "The test pattern binary is missing."
        debug "Please install tp under /opt/xensource/debug and re-run."
        debug "Exiting quietly."
        return
    fi

    ALLOCATE="thin"
    smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
        "${NFSSERVER}" "${NFSSERVERPATH}" \
        "${USE_IQN_INITIATOR_ID}" "${LISCSI_TARGET_IP}" "${LISCSI_TARGET_ID}" \
        "${LISCSI_ISCSI_ID}" "${NAPP_TARGET}" "${NAPP_USER}" "${NAPP_PASSWD}" \
	"${NAPP_AGGR}" "${NAPP_FVOLS}" "${NAPP_SIZE}" "${EQL_TARGET}" \
	"${EQL_USER}" "${EQL_PASSWD}" "${EQL_SPOOL}"
    test_exit 1 

    for i in `seq 1 ${LOOP}`; do
        debug && debug "Iteration #${i}-->"

        # Create First test disk
        smAddVdi ${SR_ID} ${DEVSTRING} "TestVDI" ${VSIZE}
        test_exit 1

        SRC_VDI1=${VDI_ID}

        smCreateVbd ${VDI_ID} ${DOM0_ID} ${DEVICE1} 
        test_exit 1

        smPlugVbd ${VBD_ID}
        test_exit 1
        VBD1=${VBD_ID}

        # Create Second test disk
        smAddVdi ${SR_ID} ${DEVSTRING} "TestVDI" ${VSIZE}
        test_exit 1

        SRC_VDI2=${VDI_ID}

        smCreateVbd ${VDI_ID} ${DOM0_ID} ${DEVICE2} 
        test_exit 1

        smPlugVbd ${VBD_ID}
        test_exit 1
        VBD2=${VBD_ID}

        # Insert same test pattern on both
        CHUNKSZ=1
        PAD=0
        ITER=$((($VDI_SIZE - $PAD)/ ($CHUNKSZ*2*1024)))
        generate_pattern ${DEV_NAME1} $CHUNKSZ $ITER $PAD
        generate_pattern ${DEV_NAME2} $CHUNKSZ $ITER $PAD
        test_exit 1

        verify_match ${DEV_NAME1} ${DEV_NAME2}
        test_exit 1

        if [[ ${SUBSTRATE_TYPE} = "netapp" ]] ; then
            COOKIE=`uuidgen`
        else
            COOKIE=""
        fi
        smVDISnap ${SRC_VDI1} ${COOKIE}
        test_exit 1
        SNAP_VDI1=${VDI_ID}

        # Remove disk pattern on DEVICE2
        zero_disk ${DEV_NAME2}
        test_exit 0

        verify_nomatch ${DEV_NAME1} ${DEV_NAME2}
        test_exit 1

        if [[ ${SUBSTRATE_TYPE} = "netapp" ]] ; then
            # Snap disk 2 using previous cookie
            smVDISnap ${SRC_VDI2} ${COOKIE}
            test_exit 1
            SNAP_VDI2=${VDI_ID}
        else
            smVDISnap ${SRC_VDI2}
            test_exit 1
            SNAP_VDI2=${VDI_ID}
        fi

        # Attach the 2 SNAP disks
        smCreateROVbd ${SNAP_VDI1} ${DOM0_ID} ${DEVICE3} 
        test_exit 1

        smPlugVbd ${VBD_ID}
        test_exit 1
        VBD3=${VBD_ID}

        smCreateROVbd ${SNAP_VDI2} ${DOM0_ID} ${DEVICE4} 
        test_exit 1

        smPlugVbd ${VBD_ID}
        test_exit 1
        VBD4=${VBD_ID}

        if [[ ${SUBSTRATE_TYPE} = "netapp" ]] ; then
            # SNAP disks should match, VDI2 should not match SNAP2
            verify_match ${DEV_NAME3} ${DEV_NAME4}
            test_exit 1

            verify_match ${DEV_NAME1} ${DEV_NAME4}
            test_exit 1

            verify_nomatch ${DEV_NAME2} ${DEV_NAME4}
            test_exit 1 
        else
            # SNAP disks should not match, VDI2 should match SNAP2
            verify_nomatch ${DEV_NAME3} ${DEV_NAME4}
            test_exit 1

            verify_nomatch ${DEV_NAME1} ${DEV_NAME4}
            test_exit 1

            verify_match ${DEV_NAME2} ${DEV_NAME4}
            test_exit 1 
        fi

        # Cleanup
        smUnplugVbd ${VBD1}
        smUnplugVbd ${VBD2}
        smUnplugVbd ${VBD3}
        smUnplugVbd ${VBD4}
        test_exit 1

        smDeleteVbd ${VBD1}
        smDeleteVbd ${VBD2}
        smDeleteVbd ${VBD3}
        smDeleteVbd ${VBD4}
        test_exit 1

        if [[ ${SUBSTRATE_TYPE} = "equal" ]] ; then
            smDeleteVdi ${SR_ID} ${DEVSTRING} ${SNAP_VDI1}
            smDeleteVdi ${SR_ID} ${DEVSTRING} ${SNAP_VDI2}
            smDeleteVdi ${SR_ID} ${DEVSTRING} ${SRC_VDI1}
            smDeleteVdi ${SR_ID} ${DEVSTRING} ${SRC_VDI2}
            test_exit 1
        else
            smDeleteVdi ${SR_ID} ${DEVSTRING} ${SRC_VDI1}
            smDeleteVdi ${SR_ID} ${DEVSTRING} ${SRC_VDI2}
            smDeleteVdi ${SR_ID} ${DEVSTRING} ${SNAP_VDI1}
            smDeleteVdi ${SR_ID} ${DEVSTRING} ${SNAP_VDI2}
            test_exit 1
        fi

    done

    unset VMHINT

    smDelete ${SR_ID}
    test_exit 0
}

# Performs some data integrity tests on domU: creates an SR & sets it as the
# default one, installs a VM, boots it, adds a VDI, runs the biotest, and
# finally deletes the VDI, the VM, and the SR.
run_domU_data_integrity_tests() {
    debug "TG: DomU Data Integrity Tests"                             
    DEVICE="xvdc"
    DEV_NAME="/dev/${DEVICE}"

    CARBON_DOMU_PASSWORD="xensource"

    smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
        "${NFSSERVER}" "${NFSSERVERPATH}" \
        "${USE_IQN_INITIATOR_ID}" "${LISCSI_TARGET_IP}" "${LISCSI_TARGET_ID}" \
        "${LISCSI_ISCSI_ID}" "${NAPP_TARGET}" "${NAPP_USER}" "${NAPP_PASSWD}" \
	"${NAPP_AGGR}" "${NAPP_FVOLS}" "${NAPP_SIZE}" "${EQL_TARGET}" \
	"${EQL_USER}" "${EQL_PASSWD}" "${EQL_SPOOL}"
    test_exit 1

    #Store the old default SR_ID
    smGetDefaultSR
    test_exit 1

    local OLD_SR_ID=$SR_DEFAULT_ID

    smSetDefaultSR ${SR_ID}
    test_exit 1

    # start guest domain
    NAME="perfVM.$DATE"
    pinstall_VM ${NAME} 256 $SR_ID

    VM_ID=${GLOBAL_RET}
    test_exit 1

    smBootVM ${VM_ID}
    test_exit 1

    getVMIP ${VM_ID} 0
    DOMU_IP=${GLOBAL_RET}

    install_ssh_certificate ${DOMU_IP} ${SSH_PRIVATE_KEY} ${CARBON_DOMU_PASSWORD}

    smAddVdi ${SR_ID} ${DEVSTRING} "TestVDI" ${VDI_SIZE}
    test_exit 1

    smCreateVbd ${VDI_ID} ${VM_ID} ${DEVICE} 
    test_exit 1

    smPlugVbd ${VBD_ID}
    test_exit 1

    #################################
    
    DOMU_REM_CMD="ssh -q -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null -i ${SSH_PRIVATE_KEY} root@${DOMU_IP}"
    if [ -n "$FAST" ] ; then
        DATAMB=32
    else
        DATAMB=128
    fi

    biotest "$DOMU_REM_CMD" $DEV_NAME $DATAMB
    
    #################################
    
    smUnplugVbd ${VBD_ID}
    test_exit 1

    smDeleteVbd ${VBD_ID}
    test_exit 1

    smDeleteVdi ${SR_ID} ${DEVSTRING} ${VDI_ID}
    test_exit 1

    smStopVM ${VM_ID}
    test_exit 1

    smUninstallVM ${SR_ID} ${VM_ID}
    test_exit 1
    
    smSetDefaultSR ${OLD_SR_ID}
    test_exit 0

    smDelete ${SR_ID}
    test_exit 1
}

# Performs the tests.
# Arguments:
# 1: test type (lvhd, lvm, ext, nfs, lvmoiscsi, extoiscsi)
run_tests() {
    if [ -z $1 ] ; then
        debug "Please supply a test type (lvhd/lvm/ext/nfs/lvmoiscsi/extoiscsi)"
        return 1
    fi

    USE_CHAP=0

    if [ $1 = "lvhd" ] ; then
        #test lvhdd
        DRIVER_TYPE=lvhd
        SUBSTRATE_TYPE=lvhd
        CONTENT_TYPE=local_lvhd
        VDI_SIZE=$((20*1024*1024*1024))
    elif [ $1 = "lvm" ] ; then
        #test lvm
        DRIVER_TYPE=lvm
        SUBSTRATE_TYPE=lvm
        CONTENT_TYPE=local_lvm
        VDI_SIZE=$((20*1024*1024*1024))
    elif [ $1 = "ext" ] ; then
        #test file
        DRIVER_TYPE=file
        SUBSTRATE_TYPE=ext
        CONTENT_TYPE=local_file
        VDI_SIZE=$((20*1024*1024*1024))
    elif [ $1 = "nfs" ] ; then
        #test file
        DRIVER_TYPE=nfs
        SUBSTRATE_TYPE=nfs
        CONTENT_TYPE=nfs
        VDI_SIZE=$((20*1024*1024*1024))
    elif [[ $1 = "lvmoiscsi"  || $1 = "lvmoiscsi_chap" ]] ; then
        # test lvm over iscsi
        DRIVER_TYPE=lvmoiscsi    
        SUBSTRATE_TYPE=lvmoiscsi
        CONTENT_TYPE=user
        VDI_SIZE=$((10*1024*1024*1024))
        if [ $1 = "lvmoiscsi" ] ; then
            USE_IQN_INITIATOR_ID="${IQN_INITIATOR_ID}"
        else
            USE_IQN_INITIATOR_ID="${IQN_INITIATOR_ID_CHAP}"
            USE_CHAP=1
        fi
    elif [ $1 = "extoiscsi" ] ; then
        # test file over iscsi
        DRIVER_TYPE=extoiscsi    
        SUBSTRATE_TYPE=extoiscsi
        CONTENT_TYPE=user
        VDI_SIZE=$((10*1024*1024*1024))
    elif [[ $1 = "netapp"  || $1 = "netapp_chap" ]] ; then
        # test the netapp driver
        DRIVER_TYPE=netapp   
        SUBSTRATE_TYPE=netapp
        CONTENT_TYPE=user
        VDI_SIZE=$((10*1024*1024*1024))
        if [ $1 = "netapp" ] ; then
            USE_IQN_INITIATOR_ID="${IQN_INITIATOR_ID}"
        else
            USE_IQN_INITIATOR_ID="${IQN_INITIATOR_ID_CHAP}"
            USE_CHAP=1
        fi
    elif [ $1 = "equal" ] ; then
        # test the EqualLogic driver
        DRIVER_TYPE=equal   
        SUBSTRATE_TYPE=equal
        CONTENT_TYPE=user
        VDI_SIZE=$((10*1024*1024*1024))
        USE_IQN_INITIATOR_ID="${IQN_INITIATOR_ID}"
    else
        debug "Unknown test type: $1.
I only know about lvhd/lvm/ext/nfs/lvmoiscsi/extoiscsi/netapp/equal"
        return 1
    fi
 
    debug ""
    debug "Running tests on substrate type:         ${SUBSTRATE_TYPE}"
    debug "                    driver type:         ${DRIVER_TYPE}"
    if [ $DRIVER_TYPE = "nfs" ] ; then
        debug "                NFS server path:         ${NFSSERVER}:${NFSSERVERPATH}"
    else
        debug "                         device:         ${DEVSTRING}"
    fi

    debug ""

    run_basic_sr_validation_tests

    if [[ $1 = "netapp"  || $1 = "netapp_chap" || $1 = "equal" ]] ; then
	run_vdi_snapshot_tests
    fi

    run_advanced_sr_validation_tests $SUBSTRATE_TYPE

    run_dom0_data_integrity_tests

    run_domU_data_integrity_tests

    run_dom0_performance_tests

    run_domU_performance_tests

    run_dom0_stress_tests
    
}

# Creates a dummy SR.
# Arguments:
# 1: the name of the SR
create_dummy() {
    copy_lvm_plugin $1
    RC=$?

    test_exit 2

    if [ "$RC" -ne 2 ] ; then
        restart_xapi
        test_exit 1
    fi

}

# Creates a dummy SR and performs the basic SR validation tests.
create_and_run_dummy_sr()
{
    DRIVER_TYPE=lvm
    SUBSTRATE_TYPE=dummylvm
    CONTENT_TYPE=local_lvm
    VDI_SIZE=$((20*1024*1024*1024))

    debug ""
    debug "Running tests on substrate type:         ${SUBSTRATE_TYPE}"
    debug "                    driver type:         ${DRIVER_TYPE}"
    debug "                         device:         ${DEVSTRING}"

    create_dummy ${SUBSTRATE_TYPE}

    run_basic_sr_validation_tests

}

TEMPLATE_ALIAS=windows

# TODO put in a main function?

process_arguments $@

post_process

check_req_args

check_req_sw

# TODO which machine is set up for passwordless logins? Isn't dom0 the machine
# on which this script is being executed?
install_ssh_certificate ${REMHOSTNAME} ${SSH_PRIVATE_KEY} ${PASSWD}

print_version_info

gen_hostlist

if [ -z "${RUN_LVHD}" ]; then
    debug "Skipping LVHD tests"
else
    run_tests "lvhd"
fi

if [ -n "${SKIP_LVM}" ]; then
    debug "Skipping LVM tests because of SKIP_LVM"
else
    run_tests "lvm"
fi

if [ -n "${SKIP_EXT}" ]; then
    debug "Skipping EXT tests because of SKIP_EXT"
else
    run_tests "ext"
fi

if [[ -z ${NFSSERVER}  || -z ${NFSSERVERPATH} ]] ; then
    debug "No NFS information specified. Skip NFS tests"
else
    echo ""
    run_tests "nfs"
fi

if [[ -z ${IQN_INITIATOR_ID} || -z ${LISCSI_TARGET_IP} || -z ${LISCSI_TARGET_ID} || -z ${LISCSI_ISCSI_ID} ]]; then
    debug "iSCSI configuration information missing. Skip iSCSI tests"
else
    echo ""
    run_tests "lvmoiscsi"

    if [ ! -z ${IQN_INITIATOR_ID_CHAP} ] ; then
        run_tests "lvmoiscsi_chap"
    fi
fi

if [[ -z ${NAPP_TARGET} || -z ${NAPP_USER} || -z ${NAPP_PASSWD} || -z ${NAPP_AGGR} || -z ${NAPP_SIZE} ]]; then
    debug "Netapp configuration information missing. Skip netapp SR tests"
else
    if [ -z ${NAPP_FVOLS} ]; then
        NAPP_FVOLS=8
    fi
    echo ""
    run_tests "netapp"

    if [ ! -z ${IQN_INITIATOR_ID_CHAP} ] ; then
        run_tests "netapp_chap"
    fi
fi

if [[ -z ${EQL_TARGET} || -z ${EQL_USER} || -z ${EQL_PASSWD} || -z ${EQL_SPOOL} ]]; then
    debug "EqualLogic configuration information missing. Skip equal SR tests"
else
    echo ""
    run_tests "equal"
fi

print_exit_info

