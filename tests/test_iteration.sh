#!/bin/bash
## Set of SM tests for default SR

## Source the general system function file
. ./XE_api_library.sh

## source performance tests
. ./performance_functions.sh

MAX_TESTS_CALCULATED=0

setup_sr()
{
    smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
        "${NFSSERVER}" "${NFSSERVERPATH}" \
        "${USE_IQN_INITIATOR_ID}" "${LISCSI_TARGET_IP}" "${LISCSI_TARGET_ID}" \
        "${LISCSI_ISCSI_ID}" "${NAPP_TARGET}" "${NAPP_USER}" "${NAPP_PASSWD}" \
	"${NAPP_AGGR}" "${NAPP_FVOLS}" "${NAPP_SIZE}" "${EQL_TARGET}" \
	"${EQL_USER}" "${EQL_PASSWD}" "${EQL_SPOOL}"
    test_exit 1

    smSetDefaultSR ${SR_ID}
}

delete_sr()
{
    smDelete ${SR_ID} 
    test_exit 1
}

create_delete_one_sr() 
{

    setup_sr

    #smGetParams ${SR_ID} test_exit 1

    delete_sr

}

# Creates and deletes $NUM_TESTS SRs.
# Arguments: none.
create_delete_sr()
{
    debug "TG: Create/Delete SR Iteration Test"
    for i in `seq 1 $NUM_TESTS` ; do
        debug "INFO: Running iteration $i"
        create_delete_one_sr
    done
}

# Creates a VDI on the currently selected SR.
# Arguments: none.
create_vdi()
{
    smAddVdi ${SR_ID} ${DEVSTRING} "TestVDI" ${VDI_SIZE}
    test_exit 1
}

# Deletes the selected VDI from the seleceted SR.
# Arguments: none.
delete_vdi()
{
    smDeleteVdi ${SR_ID} ${DEVSTRING} ${VDI_ID}
    test_exit 1
}


# Creates and immediately deletes a VDI.
# Arguments: none.
create_delete_one_vdi()
{
    create_vdi

    delete_vdi
}

# Creates and immediately deletes $NUM_TESTS VDIs.
# Arguments: none.
create_delete_vdi () 
{
    debug "TG: Create/Delete VDI Iteration Test"
    setup_sr
    
    for i in `seq 1 $NUM_TESTS` ; do
        debug "INFO: Running iteration $i"
        create_delete_one_vdi
    done

    delete_sr
}

# Calculates the maximum number of loops so that all VDIs can fit in the SR.
# Arguments: none.
calculate_max_number()
{
    if [ ${MAX_TESTS_CALCULATED} -eq 1 ] ; then
        echo "returning"
    fi

    smGetPhysicalSize ${SR_ID}
    PHYS_SIZE="${GLOBAL_RET}" # in bytes
    echo "PHYS_SIZE = $PHYS_SIZE"

    MAX_NUMBER_LOOPS=$(($PHYS_SIZE/$VDI_SIZE))

    if [ $NUM_TESTS -gt $MAX_NUMBER_LOOPS ] ; then
        debug "Number of tests requested too large ($NUM_TESTS). Automatically adjusting to $MAX_NUMBER_LOOPS"
        NUM_TESTS=$MAX_NUMBER_LOOPS
    fi
    MAX_TESTS_CALCULATED=1
}

# First create some VDIs and then delete them all.
# Arguments: none.
create_many_vdi()
{
    debug "TG: Create/Delete VDI Iteration Test"
    VDI_SIZE=$((4*1024*1024))
    declare -a VDI_IDS

    setup_sr

    calculate_max_number
    calculate_max_number # FIXME why the 2nd call?

    for i in `seq 1 $NUM_TESTS` ; do
        create_vdi
        VDI_IDS[$i]="${VDI_ID}"
    done

    for i in `seq 1 $NUM_TESTS` ; do
        VDI_ID=${VDI_IDS[$i]}
        delete_vdi
    done

    delete_sr
}

# Initializes the $DEV_IDS array with "xvd[c..p]".
# Arguments: none.
fill_dev_ids()
{
    i=1

    # ugly loop to fill an array of xvd*'s
    # TODO replace with "echo {c..p}"
    for c in a b c d e f g h i j k l m n o p ; do
        DEV_IDS[$i]="xvd$c"
        i=`expr $i + 1`
    done

    # not necessary, we only have up to xvdp
#    for c in a b c d e f g h i j k l m n o p q r s t u v w x y z ; do
#        for d in a b c d e f g h i j k l m n o p q r s t u v w x y z ; do
#            DEV_IDS[$i]="xvd$c$d"
#            i=`expr $i + 1`
#        done
#    done
}

# Create some VDIs and VBDs, and then delete them all.
# Arguments: none.
create_many_vbd()
{
    debug "TG: Create/Delete VBD Iteration Test"
    VDI_SIZE=$((4*1024*1024))
    MY_NUM_TESTS=15

    declare -a VDI_IDS
    declare -a DEV_IDS
    declare -a VBD_IDS

    setup_sr

    calculate_max_number

    fill_dev_ids

    for i in `seq 1 $MY_NUM_TESTS` ; do
        create_vdi
        VDI_IDS[$i]="${VDI_ID}"
    done

    for i in `seq 1 $MY_NUM_TESTS` ; do
        VDI_ID=${VDI_IDS[$i]}
        DEVICE=${DEV_IDS[$i]}
        DEV_NAME="/dev/${DEVICE}"
        create_vbd "${DOM0_ID}"
        VBD_IDS[$i]="${VBD_ID}"
    done

    for i in `seq 1 $MY_NUM_TESTS` ; do
        VBD_ID=${VBD_IDS[$i]}
        delete_vbd
    done

    for i in `seq 1 $MY_NUM_TESTS` ; do
        VDI_ID=${VDI_IDS[$i]}
        delete_vdi
    done

    delete_sr
}

# Create some VDIs, create some VBDs, plug the VBDs, unplug the VBDs, delete the
# VBDs, delete the VDIs.
# Arguments:
# 1: UUID of the VM where the VDI will be attached
# 2: devices to skip for non-dom0 VMs (xvda and xvdb are already used)
plug_many_vbd_in_dom()
{
    debug "TG: Plug/Unplug VBD Iteration Test"
    VDI_SIZE=$((4*1024*1024))

    local _VMID="$1"
    local _DEV_SHIFT=$2 # how many devices to shift
                        # xvda and b are in use already in guest

    MY_NUM_TESTS=$((16-$_DEV_SHIFT))  #stop at xvdp

    declare -a VDI_IDS
    declare -a DEV_IDS
    declare -a VBD_IDS

    #calculate_max_number

    fill_dev_ids

    for i in `seq 1 $MY_NUM_TESTS` ; do
        create_vdi
        VDI_IDS[$i]="${VDI_ID}"
    done

    for i in `seq 1 $MY_NUM_TESTS` ; do
        VDI_ID=${VDI_IDS[$i]}
        DEVICE=${DEV_IDS[$(($i+$_DEV_SHIFT))]}
        DEV_NAME="/dev/${DEVICE}"
        create_vbd "${_VMID}"
        echo "created VBD with dev-name=$DEV_NAME"
        VBD_IDS[$i]="${VBD_ID}"
    done

    for i in `seq 1 $MY_NUM_TESTS` ; do
        VBD_ID=${VBD_IDS[$i]}
        plug_vbd
    done

    for i in `seq 1 $MY_NUM_TESTS` ; do
        VBD_ID=${VBD_IDS[$i]}
        unplug_vbd
    done

    for i in `seq 1 $MY_NUM_TESTS` ; do
        VBD_ID=${VBD_IDS[$i]}
        delete_vbd
    done

    for i in `seq 1 $MY_NUM_TESTS` ; do
        VDI_ID=${VDI_IDS[$i]}
        delete_vdi
    done
}

# On dom0, create an SR, create, plug, unplug, and delete a few VBDs, delete
# the SR.
# Arguments: none.
plug_many_vbd_in_dom0()
{
    setup_sr
    plug_many_vbd_in_dom ${DOM0_ID} 0
    delete_sr
}

# On dom${, create an SR, create, plug, unplug, and delete a few VBDs, delete
# the SR.
# Arguments: none.
plug_many_vbd_in_domU()
{
    setup_sr
    install_guest
    start_guest

    plug_many_vbd_in_dom ${VM_ID} 2

    stop_guest
    delete_guest
    delete_sr
}

# Create a VBD for $VDI_ID, using $DEVICE as the device number.
# Arguments:
# 1: the UUID of the VM
create_vbd()
{
    smCreateVbd ${VDI_ID} $1 ${DEVICE} 
    test_exit 1
}

# Deletes the $VBD_ID VBD.
# Arguments: none.
delete_vbd()
{
    smDeleteVbd ${VBD_ID}
    test_exit 1
}

# Creates a VBD and then deletes it on the VDI having the same ID as $VDI_ID.
# Arguments:
# 1: the UUID of the VM
create_delete_one_vbd()
{
    create_vbd $1

    delete_vbd
}

# On dom0, create an SR and a VDI, create and delete $NUM_TESTS VBDs, delete
# the VDI and the SR.
create_delete_vbd () 
{
    DEVICE="xvda"
    DEV_NAME="/dev/${DEVICE}"

    debug "TG: Create/Delete VBD Iteration Test"

    setup_sr

    create_vdi

    for i in `seq 1 $NUM_TESTS` ; do
        debug "INFO: Running iteration $i"
        create_delete_one_vbd $DOM0_ID
    done

    delete_vdi

    delete_sr
}

# Plugs the $VBD_ID.
# Arguments: none.
plug_vbd()
{
    smPlugVbd ${VBD_ID}
    test_exit 1
}

# Unplugs the $VBD_ID.
# Arguments: none.
unplug_vbd()
{
    smUnplugVbd ${VBD_ID}
    test_exit 1
}

# Plugs $VBD_ID, creates a FS, checks the FS, zeroes the partition, unplugs the
# VBD.
# Arguments:
# 1: machine access command
# ARG1 = REM_CMD
plug_unplug_one_vbd()
{
    plug_vbd

    check_vbd "$1" ${DEV_NAME} 15
    test_exit 1

    run_mkfs "$1" ${DEVICE}
    test_exit 1

    run_fsck "$1" ${DEVICE}
    test_exit 1

    run_diskwipe "$1" ${DEVICE}
    test_exit 1

    unplug_vbd
}

# On dom0, creates an SR, a VDI, and a VBD, plugs the VDB, unplugs the VBD,
# deletes the VBD, VDI, and SR.
plug_unplug_vbd_in_dom0 () 
{
    DEVICE="xvda"
    DEV_NAME="/dev/${DEVICE}"

    debug "TG: Plug/unplug VBD Dom0 Iteration Test"

    setup_sr

    create_vdi

    create_vbd $DOM0_ID

    for i in `seq 1 $NUM_TESTS` ; do
        debug "INFO: Running iteration $i"
        plug_unplug_one_vbd  "$REM_CMD"
    done

    delete_vbd

    delete_vdi

    delete_sr
}

# Installs a VM on the SR with $SR_ID.
# Arguments: none.
install_guest() 
{
    # start guest domain
    NAME="perfVM.$DATE"
    pinstall_VM ${NAME} 256 $SR_ID
    test_exit 1

    VM_ID=${GLOBAL_RET}
}

# Boots the VM with $VM_ID.
# Arguments: none.
start_guest()
{
    smBootVM ${VM_ID}
    test_exit 1

    getVMIP ${VM_ID} 0
    DOMU_IP=${GLOBAL_RET}
}

# Stops the VM with $VM_ID.
# Arguments: none.
stop_guest()
{
    smStopVM ${VM_ID}
    test_exit 0
}

# Deletes the VM with $VM_ID
# Arguments: none.
delete_guest()
{
    smUninstallVM ${SR_ID} ${VM_ID}
    test_exit 0
}

# Creates an SR, installs & boots a VM, creates a VDI & a VBD, plugs the VBD,
# performs some basic tests, unplugs the VBD, and finally deletes the VBD, VDI,
# VM, and SR.
plug_unplug_vbd_in_domU () 
{
    DEVICE="xvdc"
    DEV_NAME="/dev/${DEVICE}"

    debug "TG: Plug/unplug VBD DomU Iteration Test"

    setup_sr

    install_guest

    start_guest

    getVMIP ${VM_ID} 0
    DOMU_IP=${GLOBAL_RET}
    DOMU_REM_CMD="ssh -q -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null -i ${SSH_PRIVATE_KEY} root@${DOMU_IP}"
    CARBON_DOMU_PASSWORD="xensource"

    install_ssh_certificate ${DOMU_IP} ${SSH_PRIVATE_KEY} ${CARBON_DOMU_PASSWORD}

    create_vdi

    create_vbd $VM_ID

    for i in `seq 1 $NUM_TESTS` ; do
        debug "INFO: Running iteration $i"
        plug_unplug_one_vbd "$DOMU_REM_CMD"
    done

    delete_vbd

    delete_vdi

    stop_guest

    delete_guest

    delete_sr
}

# Performs the following tests on the specified SR type:
# create_delete_sr, create_delete_vdi, create_delete_vbd,
# plug_unplug_vbd_in_dom0, plug_unplug_vbd_in_domU, create_many_vdi,
# create_many_vbd, plug_many_vbd_in_dom0, plug_many_vbd_in_domU.
# Arguments:
# 1: type of the SR (lvhd, lvm, ext, nfs, lvmoiscsi, extoiscsi)
# TODO prefix test functions with "test_" so it can be clear what is tested.
run_tests()
{
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

    create_delete_sr

    create_delete_vdi

    create_delete_vbd

    plug_unplug_vbd_in_dom0

    plug_unplug_vbd_in_domU

    create_many_vdi

    create_many_vbd

    plug_many_vbd_in_dom0

    plug_many_vbd_in_domU

}

# main: run all tests on all SRs, skipping the SRs the caller disabled.

TEMPLATE_ALIAS=windows

process_arguments $@

post_process

check_req_args
    
if [ -z "$NUM_TESTS" ] ; then
    NUM_TESTS=25
fi

check_req_sw

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

