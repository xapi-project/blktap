#!/bin/bash
## Set of SM tests for default SR

## Source the general system function file
. ./XE_api_library.sh

## source performance tests
. ./performance_functions.sh

DD_HAS_RUN=0
BONNIE_HAS_RUN=0
TMPUUID=`uuidgen`
VMIDS_FILE="/tmp/vmids-${TMPUUID}.txt"
TEST_FAILED_FILE="/tmp/test_fail"

# FIXME redundant
replug_vbd()
{
    local VBD_ID=$1

    smUnplugVbd ${VBD_ID}
    test_exit 1
    smPlugVbd ${VBD_ID}
    test_exit 1

    sleep 1
}

# FIXME redundant
setup_sr() 
{
    if [ -z ${SR_ID} ]; then
	smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
            "${NFSSERVER}" "${NFSSERVERPATH}" "${USE_IQN_INITIATOR_ID}" \
            "${LISCSI_TARGET_IP}" "${LISCSI_TARGET_ID}" "${LISCSI_ISCSI_ID}" \
            "${NAPP_TARGET}" "${NAPP_USER}" "${NAPP_PASSWD}" "${NAPP_AGGR}" \
            "${NAPP_FVOLS}" "${NAPP_SIZE}" "${EQL_TARGET}" "${EQL_USER}" \
            "${EQL_PASSWD}" "${EQL_SPOOL}"
        test_exit 1

        CLEANUP_SR=1
    else
        smCheckSR ${SR_ID}
        test_exit 1
        CLEANUP_SR=0
    fi

    #Store the old default SR_ID
    smGetDefaultSR

    local OLD_SR_ID=$SR_DEFAULT_ID
    
    smSetDefaultSR ${SR_ID}

}

# FIXME redundant
cleanup_sr()
{
    smSetDefaultSR ${OLD_SR_ID}
    if [ ${CLEANUP_SR} == 1 ]; then
        smDelete ${SR_ID}
        test_exit 0
        unset SR_ID
    fi
}


install_stress_vm() 
{
    # start guest domain
    CURDATE=`date +%s-%N`
    NAME="perfVM.$CURDATE"
    pinstall_VM ${NAME} 256 $SR_ID 
    test_exit 1

    VM_IDS[$1]=${GLOBAL_RET}
    echo "Installed VM [${GLOBAL_RET}]"
    
    echo "$GLOBAL_RET" >> $VMIDS_FILE
}

insert_ssh_certificate()
{
    local local_VBD_ID

    smCreateVbd $1 ${DOM0_ID} 'autodetect'
    test_exit 1
    local_VBD_ID=${VBD_ID}

    smPlugVbd ${local_VBD_ID}
    test_exit 1

    smGetVbdDevice ${local_VBD_ID}
    test_exit 1

    Devpath="/dev/${VBD_DEVICE}1"
    Mntpath="/mnt/${local_VBD_ID}"

    cmd="$REM_CMD mkdir ${Mntpath} && mount ${Devpath} ${Mntpath}"
    run "$cmd"
    if [ $RUN_RC -gt 0 ]; then
        incr_exitval
    fi

    cmd="$REM_CMD cp -Rf /root/.ssh ${Mntpath}/root/."
    run "$cmd"
    if [ $RUN_RC -gt 0 ]; then
        incr_exitval
    fi

    cmd="$REM_CMD umount ${Mntpath} && rm -Rf ${Mntpath}"
    run "$cmd"
    if [ $RUN_RC -gt 0 ]; then
        incr_exitval
    fi
    test_exit 1

    smUnplugVbd ${local_VBD_ID}
    test_exit 1

    smDeleteVbd ${local_VBD_ID}
    test_exit 1
}

boot_stress_vm()
{
    local VM_ID=${VM_IDS[$1]}

    debug "Boot VM ID is ${VM_ID}"

    if [ ! -z $NO_EXPECT ]; then
        getprimaryVMVDI ${VM_ID}
        test_exit 1
        
        PRIMARY=${GLOBAL_RET}
        debug "Inserting certificate on device ${PRIMARY}"
        insert_ssh_certificate ${PRIMARY}
    fi

    debug "Booting VM [${VM_ID}]"
    smBootVM ${VM_ID}
    test_exit 1

    getVMIP ${VM_ID} 0
    test_exit 1

    local DOMU_IP="$GLOBAL_RET"

    if [ -z $NO_EXPECT ]; then
        install_ssh_certificate ${DOMU_IP} ${SSH_PRIVATE_KEY} ${CARBON_DOMU_PASSWORD}
        test_exit 1
    fi

    smAddVdi ${SR_ID} ${DEVSTRING} "TestVDI" ${VDI_SIZE}
    test_exit 1

    VDI_IDS[$1]=$VDI_ID

    smCreateVbd ${VDI_ID} ${VM_ID} ${DEVICE} 
    test_exit 1

    VBD_IDS[$1]=$VBD_ID

    smPlugVbd ${VBD_ID}
    test_exit 1

}

run_fsx_stress_test()
{
    run_fsx "$1" ${DEV_NAME}  ${NUM_ITER}

    if [ $EXITVAL -ne 0 ] ; then
        touch $TEST_FAILED_FILE
        exit 1
    fi

    test_exit 1
}

verify_stress_disk()
{
    local VM_ID=${VM_IDS[$1]}
    local local_VBD_ID=${VBD_IDS[$1]}
    local VDI_ID=${VDI_IDS[$1]}

    smUnplugVbd ${local_VBD_ID}
    test_exit 1

    smDeleteVbd ${local_VBD_ID}
    test_exit 1


    # Reattach VDI to Dom0 to verify
    smCreateVbd ${VDI_ID} ${DOM0_ID} 'autodetect' 
    test_exit 1
    local_VBD_ID=${VBD_ID}

    smPlugVbd ${local_VBD_ID}
    test_exit 1

    smGetVbdDevice ${local_VBD_ID}
    test_exit 1

    run_fsck "$REM_CMD" ${VBD_DEVICE}
    test_exit 1

    smUnplugVbd ${local_VBD_ID}
    test_exit 1

    smDeleteVbd ${local_VBD_ID}
    test_exit 1

    smCreateVbd ${VDI_ID} ${VM_ID} ${DEVICE} 
    test_exit 1
    VBD_IDS[$1]=$VBD_ID

    smPlugVbd ${VBD_ID}
    test_exit 1
}

run_postmark_stress_test()
{
    run_postmark "$1" ${DEV_NAME}  ${NUM_ITER}
    test_exit 1
}

# Runs the fsx and postmark tests on a VM.
# Arguments:
# 1: VM index
run_stress_test()
{
    local local_VBD_ID
    local local_VDI_ID
    local VM_ID=${VM_IDS[$1]}

    getVMIP ${VM_ID} 0
    test_exit 1
    local DOMU_IP="$GLOBAL_RET"

    debug "running test on VM $1 with IP $DOMU_IP"
    DOMU_REM_CMD="ssh -q -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null -i ${SSH_PRIVATE_KEY} root@${DOMU_IP}"

    run_fsx_stress_test "${DOMU_REM_CMD}"

    verify_stress_disk $1

    run_postmark_stress_test "${DOMU_REM_CMD}"

    verify_stress_disk $1

    local_VBD_ID=${VBD_IDS[$1]}
    local_VDI_ID=${VDI_IDS[$1]}
    smUnplugVbd ${local_VBD_ID}
    test_exit 1

    smDeleteVbd ${local_VBD_ID}
    test_exit 1

    smDeleteVdi ${SR_ID} ${DEVSTRING} ${local_VDI_ID}
    test_exit 1
}

cleanup_stress_vm()
{
    local VM_ID=${VM_IDS[$1]}
    local VBD_ID=${VBD_IDS[$1]}
    local VDI_ID=${VDI_IDS[$1]}

    smStopVM ${VM_ID}
    test_exit 1

    smUninstallVM ${SR_ID} ${VM_ID}
    test_exit 1
}

# Installs $NUM_TEST_VMS VMs and runs stress test on them.
run_domU_stress_test()
{
    DEVICE="xvdc"
    DEV_NAME="/dev/${DEVICE}"
    BS=4K
    if [ -z $NUM_TEST_VMS ] ; then
        NUM_VM=1
    else
        NUM_VM=$NUM_TEST_VMS
    fi
    declare -a VM_IDS
    declare -a VDI_IDS
    declare -a VBD_IDS

    if [ -n "$FAST" ] ; then
        NUM_ITER=25000
    else
        NUM_ITER=2500000
    fi

    CARBON_DOMU_PASSWORD="xensource"

    rm -f $VMIDS_FILE 
    touch $VMIDS_FILE

    rm -f $TEST_FAILED_FILE

    for i in `seq 1 $NUM_VM` ; do
        ( install_stress_vm $i ) &
        #sleep, so the name is unique (based on date)
        sleep 2
    done

    wait

    VM_IDS=( `cat $VMIDS_FILE`)

    for i in `seq 0 $(($NUM_VM-1))` ; do
        #echo "Booting VM $i - ${VM_IDS[$i]}"
        boot_stress_vm $i
    done

    for i in `seq 0 $(($NUM_VM-1))` ; do
        #echo "run stress on $i - ${VM_IDS[$i]}"
        ( run_stress_test $i ) &
    done

    wait

    if [ -e "$TEST_FAILED_FILE" ] ; then
        debug "fsx test failed"
        exit 1
    fi


    for i in `seq 0 $(($NUM_VM-1))` ; do
        #echo "cleanup stress on $i - ${VM_IDS[$i]}"
        cleanup_stress_vm $i
    done
}

# Creates an SR, performs stress tests on concurrent VMs several times, deletes
# the SR.
run_domU_stress_tests() 
{
    setup_sr

    if [ -z $NUM_TESTS ] ; then
        NUM_TESTS=25
    fi
    for i in `seq 1 $NUM_TESTS` ; do
        CUR_DATE=`date`
        debug ""
        debug "TG: Running stress run #$i (${CUR_DATE})"
        run_domU_stress_test $i
    done

    cleanup_sr
}

# Entry point.
run_tests()
{
    if [ -z $1 ] ; then
        debug "Please supply a test type (lvhd/lvm/ext/nfs)"
        return 1
    fi

    USE_CHAP=0
    local THREAD_ID=$2

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
        VDI_SIZE=$((4*1024*1024*1024))
    elif [ $1 = "ext" ] ; then
        #test file
        DRIVER_TYPE=file
        SUBSTRATE_TYPE=ext
        CONTENT_TYPE=local_file
        VDI_SIZE=$((4*1024*1024*1024))
    elif [ $1 = "nfs" ] ; then
        #test file
        DRIVER_TYPE=nfs
        SUBSTRATE_TYPE=nfs
        CONTENT_TYPE=nfs
        VDI_SIZE=$((4*1024*1024*1024))
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
    debug "Running tests on substrate type:                   ${SUBSTRATE_TYPE}"
    debug "                    driver type:                   ${DRIVER_TYPE}"
    if [ $DRIVER_TYPE = "nfs" ] ; then
        debug "                NFS server path:                   ${NFSSERVER}:${NFSSERVERPATH}"
    else
        debug "                         device:                   ${DEVSTRING}"
    fi

    debug ""


    run_domU_stress_tests $THREAD_ID
}

check_block_device_unused()
{
    smCheckPbdUnused $DEVSTRING
    test_exit 1
}

TEMPLATE_ALIAS=windows

process_arguments $@

post_process

check_req_args

check_req_sw

if [ -z $NO_EXPECT ]; then
    install_ssh_certificate ${REMHOSTNAME} ${SSH_PRIVATE_KEY} ${PASSWD}
fi

if [ -z ${SR_ID} ]; then
    check_block_device_unused
fi

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
    debug "No NFS information specified. Skip NFS test"
else
    echo ""
    run_tests "nfs" 1
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

