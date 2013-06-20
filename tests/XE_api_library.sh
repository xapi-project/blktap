#!/bin/bash

# Harness debugging support
if [ "${DEBUG_SETX}" = "yes" ]; then
    set -x
fi

## Source the system global variables file
. ./globals.sh

############################################################
## General Helper Functions
############################################################

usage()
{
    echo "Incorrect arguments to script"
    echo "Usage: $0 [VARIABLE=VALUE]"
    echo "       Where Variable may be one of:"
    echo "       TEMPLATE_ALIAS - [windows, windowsXP or debian]"
    echo "       USERNAME"
    echo "       PASSWD"
    echo "       REMHOSTNAME"
    echo "       CMD"
    echo "       REM_CMD"
    echo "       STOP_TIME"
    echo "       MAX_PERIOD"
    echo "       MY_HOSTNAME"
    echo "       IQN_INITIATOR_ID"
    echo "       IQN_INITIATOR_ID_CHAP"
    echo "       CHAP_USERNAME"
    echo "       CHAP_PASSWORD"
    echo "       LISCSI_TARGET_IP"
    echo "       LISCSI_TARGET_ID"
    echo "       LISCSI_LUN_ID"
    echo "       LISCSI_ISCSI_ID"
    echo "       INITIATORNAME_FILE"
    echo "       ISCSIADM"
    echo "       NUM_LUNS"
    echo "       NUM_TESTS"
    echo "       NUM_TEST_VMS"
    echo "       DEVSTRING"
    echo "       NFSSERVER"
    echo "       NFSSERVERPATH"
    echo "       BRIDGE"
    echo "       TEMPLATE_ALIAS"
    echo "       CLONE_DEBUG    - [0 or 1 to speed up cloning for debug]"
    echo "       FAST"
    echo "       NOCLEANUP"
    echo "       SSH_PRIVATE_KEY"
    echo "       DEBUG_FILE     - Output test results to filename"
    echo "       SR_ID  - Use an existing SR for the tests"
    echo "       TESTVAL"
    echo "       NO_EXPECT"
    echo "       SKIP_LVM"
    echo "       SKIP_DUMMYLVM"
    echo "       SKIP_EXT"
    echo "       RUN_LVHD"
    echo "       TOOLS_ROOT"
    echo "       APT_CACHER"
    echo "       VM_INSTALL_CMD"
    echo "       VM_IMAGE_LOCATION"
    echo ""
    exit 1
}

# Receives a string in the form "variable=value", and if the variable name is
# an expected one, the BASH variable is initialized to the indicated value.
process_args()
{
    out=`echo $1 | grep =`
    if [ $? -ne 0 ]; then
        debug "Arguments must be in the form 'VARIABLE=VALUE'"
        exit
    fi
    VAR=`echo $1 | cut -d= -f1`
    VAL=`echo $1 | cut -d= -f2`

    case $VAR in
        USERNAME) USERNAME=$VAL;;
        PASSWD) PASSWD=$VAL;;
        REMHOSTNAME) REMHOSTNAME=$VAL;;
        CMD) CMD=$VAL;;
        REM_CMD) REM_CMD=$VAL;;
        STOP_TIME) STOP_TIME=$VAL;;
        MAX_PERIOD) MAX_PERIOD=$VAL;;
        MY_HOSTNAME) MY_HOSTNAME=$VAL;;
        IQN_INITIATOR_ID) IQN_INITIATOR_ID=$VAL;;
        IQN_INITIATOR_ID_CHAP) IQN_INITIATOR_ID_CHAP=$VAL;;
        CHAP_USERNAME) CHAP_USERNAME=$VAL;;
        CHAP_PASSWORD) CHAP_PASSWORD=$VAL;;
        LISCSI_TARGET_IP) LISCSI_TARGET_IP=$VAL;;
        LISCSI_TARGET_ID) LISCSI_TARGET_ID=$VAL;;
        LISCSI_LUN_ID) LISCSI_LUN_ID=$VAL;;
        LISCSI_ISCSI_ID) LISCSI_ISCSI_ID=$VAL;;
        INITIATORNAME_FILE) INITIATORNAME_FILE=$VAL;;
        ISCSIADM) ISCSIADM=$VAL;;
        NAPP_TARGET) NAPP_TARGET=$VAL;;
        NAPP_USER) NAPP_USER=$VAL;;
        NAPP_PASSWD) NAPP_PASSWD=$VAL;;
        NAPP_AGGR) NAPP_AGGR=$VAL;;
        NAPP_FVOLS) NAPP_FVOLS=$VAL;;
        NAPP_SIZE) NAPP_SIZE=$VAL;;
        EQL_TARGET) EQL_TARGET=$VAL;;
        EQL_USER) EQL_USER=$VAL;;
        EQL_PASSWD) EQL_PASSWD=$VAL;;
        EQL_SPOOL) EQL_SPOOL=$VAL;;
        NUM_LUNS) NUM_LUNS=$VAL;;
        NUM_TESTS) NUM_TESTS=$VAL;;
        NUM_TEST_VMS) NUM_TEST_VMS=$VAL;;
        DEVSTRING) DEVSTRING=$VAL;;
        NFSSERVERPATH) NFSSERVERPATH=$VAL;;
        NFSSERVER) NFSSERVER=$VAL;;
        TEMPLATE_ALIAS) TEMPLATE_ALIAS=$VAL;;
        CLONE_DEBUG) CLONE_DEBUG=$VAL;;
        FAST) FAST=$VAL;;
        NOCLEANUP) NOCLEANUP=$VAL;;
        SSH_PRIVATE_KEY) SSH_PRIVATE_KEY=$VAL;;
        DEBUG_FILE) DEBUG_FILE=$VAL;;
        SR_ID) SR_ID=$VAL;;
        ONHOST) ONHOST=$VAL;;
        SHARED_HBA) SHARED_HBA=1;;
        MAXSEQ) MAXSEQ=$VAL;;
        MAXLOOP) MAXLOOP=$VAL;;
        TESTVAL) TESTVAL=$VAL;;
        NO_EXPECT) NO_EXPECT=1;;
        BRIDGE) BRIDGE=$VAL;;
        SKIP_LVM) SKIP_LVM=$VAL;;
        SKIP_DUMMYLVM) SKIP_DUMMYLVM=$VAL;;
        SKIP_EXT) SKIP_EXT=$VAL;;
        RUN_LVHD) RUN_LVHD=$VAL;;
        TOOLS_ROOT) TOOLS_ROOT=$VAL;;
        APT_CACHER) APT_CACHER=$VAL;;
        VM_INSTALL_CMD) VM_INSTALL_CMD=$VAL;;
        VM_IMAGE_LOCATION) VM_IMAGE_LOCATION=$VAL;;
        *)       echo "Not a valid variable ($VAR)"
                 return 1
                 ;;
    esac
    return 0
}

# Performs some additional initialization steps.
post_process()
{
    if [ -z ${TEMPLATE_ALIAS} ]; then
        debug "Template Alias required [TEMPLATE_ALIAS={windows,windowsXP,debian}]"
        usage
    fi

    if [ -z ${DEBUG_FILE} ]; then
        DEBUG_FILE="/tmp/$DATE-results.txt"
    fi

    if [ -z ${DISK} ]; then
        if [ ${TEMPLATE_ALIAS} == "debian" ]; then
            DISK="xvd"
        else
            DISK="hd"
        fi
    fi

    if [ ${TEMPLATE_ALIAS} == "windows" ]; then
        TEMPLATE="Windows Server 2003"
    elif [ ${TEMPLATE_ALIAS} == "windowsXP" ]; then
        TEMPLATE="Windows XP SP2"
    elif [ ${TEMPLATE_ALIAS} == "debian" ]; then
        TEMPLATE="Demo Linux VM"
    else
        debug "Incorrect template alias"
        usage
    fi
    #ARGS="-u ${USERNAME} -pw ${PASSWD} -h ${REMHOSTNAME}"
    ARGS=""
    REM_CMD="ssh -q -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null -i ${SSH_PRIVATE_KEY} root@${REMHOSTNAME}"
    REM_CMD_NOWAIT="ssh -n -f -q -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null -i ${SSH_PRIVATE_KEY} root@${REMHOSTNAME}"

    if [ ${TOOLS_ROOT} = "none" ] ; then
        FSX_WEB_LOCATION="skip"
        PMARK_BINARY_LOCATION="skip"
        PMARK_COMMANDS_LOCATION="skip"
        BIOTEST_WEB_LOCATION="skip"
    else
        FSX_WEB_LOCATION="${TOOLS_ROOT}/fsx/fsx"
        PMARK_BINARY_LOCATION="${TOOLS_ROOT}/pmark/postmark"
        PMARK_COMMANDS_LOCATION="${TOOLS_ROOT}/pmark/stress_test"
        BIOTEST_WEB_LOCATION="${TOOLS_ROOT}/biotest/biotest"
    fi
}

debug()
{
    local CURRENT_DATE_STRING=`date "+%k:%M:%S"`
    echo "$CURRENT_DATE_STRING: $1"
    echo "$CURRENT_DATE_STRING: $1" >> ${DEBUG_FILE}
    return
}

# Progress messages with colour argument
# Arg 1 -> Message
# Arg 2 -> Color
debug_test()
{
    local CURRENT_DATE_STRING=`date "+%k:%M:%S"`
    if [ $GLOBAL_DEBUG == 1 ]; then
        LEN=`echo $1 | wc -c`
        
        echo -en "$black"
        echo -en "$CURRENT_DATE_STRING: $1"
        # don't echo the dots when debug is set
        if ! `echo $- | grep -q x` ; then
            for (( dbgout = $LEN ; dbgout <= $LINE_LEN; dbgout++ ))
            do
                echo -en "."
            done
        fi
        echo -en "$CURRENT_DATE_STRING: $1" >> $DEBUG_FILE
    fi
    return
}

debug_result()
{
     if [ $GLOBAL_DEBUG == 1 ]; then

         if [ $1 == 0 ]; then
             echo -en "$green"
             echo -en " PASS"
             echo -en "\tPASS" >> $DEBUG_FILE
         elif [ $1 == 2 ]; then
             echo -en "$yellow"
             echo -en " SKIP"
             echo -en "\tSKIP" >> $DEBUG_FILE
         else
             echo -en "$red"
             echo -en " FAIL"
             echo -en "\tFAIL" >> $DEBUG_FILE
         fi
         reset
    fi
    return  
}

# Executes the supplied command. Sets the output of the command in the
# RUN_OUTPUT global variable.
run()
{
    RUN_LAST_CMD="$@"

    RUN_START_DATE=`date`
    RUN_OUTPUT=`$@`
    RUN_RC=$?
    RUN_FINISH_DATE=`date`

    return ${RUN_RC}
}

reset()
{
    if [ $GLOBAL_DEBUG == 1 ]; then
        tput sgr0
        echo
        echo >> $DEBUG_FILE
    fi
    return
}

# Increases the EXITVAL variable by one.
incr_exitval()
{
    EXITVAL=`expr ${EXITVAL} + 1`
}

# Decreases the EXITVAL variable by one.
decr_exitval()
{
    EXITVAL=`expr ${EXITVAL} - 1`
}

# If the EXITVAL global variable is greater than one and the argument supplied
# equals one, the execution of the test is stopped.
# Arg 1 -> Force Exit
test_exit()
{
    if [ ${EXITVAL} -gt 0 -a $1 == 1 ]; then
        debug "[Test Failed, not continuing]"
        debug "#############################"
        debug "Last logged message was:"
        cat ${LOGFILE} >> $DEBUG_FILE
        debug "Last run command was (exit code = $RUN_RC)  :"
        debug ""
        debug "$RUN_LAST_CMD"
        debug ""
        debug "Command output                              :"
        debug ""
        debug "$RUN_OUTPUT"
        debug ""
        debug "Command started        : $RUN_START_DATE"
        debug "Command   ended        : $RUN_FINISH_DATE"
        debug "#############################"

        host_cleanup
        exit ${EXITVAL}
    fi
    EXITVAL=0
}

test_debug()
{
    debug "[Test Failed, debug output]"
    debug "#############################"
    debug "Last logged message was:"
    cat ${LOGFILE} >> $DEBUG_FILE
    debug "Last run command was (exit code = $RUN_RC)  :"
    debug ""
    debug "$RUN_LAST_CMD"
    debug ""
    debug "Command output                              :"
    debug ""
    debug "$RUN_OUTPUT"
    debug ""
    debug "Command started        : $RUN_START_DATE"
    debug "Command   ended        : $RUN_FINISH_DATE"
    debug "#############################"
}

############################################################
## SM General Helper Functions
############################################################

# Discovers all the hosts in a pool and stores their UUIDs in POOL_HOSTS, their
# IP address in POOL_IPS, and their dom0 UUIDs in POOL_VM (TODO verify).
gen_hostlist()
{
    cmd="$REM_CMD $CMD host-list --minimal"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi

    GLOBAL_RET=$RUN_OUTPUT
    POOL_INDEX=0
    for i in `echo $RUN_OUTPUT | sed 's/,/\n/g'`; do
        cmd="$REM_CMD $CMD host-list uuid=${i} params=address --minimal"
        run $cmd
        POOL_HOSTS[$POOL_INDEX]=$i
        POOL_IPS[$POOL_INDEX]=$RUN_OUTPUT
        cmd="$REM_CMD $CMD vm-list resident-on=${i} is-control-domain=true --minimal"
        run $cmd
        POOL_VM[$POOL_INDEX]=$RUN_OUTPUT
        POOL_INDEX=`expr $POOL_INDEX + 1`
    done
    return
}

# Sets the iSCSI IQN of a host.
# Arguments:
# 1: the host UUID
# 2: the new IQN
set_iscsiIQN()
{
    cmd="$REM_CMD $CMD host-param-set uuid=${1} other-config-iscsi_iqn=\"${2}\""
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
	debug_result 1
	incr_exitval
	return 1
    fi
    return 0
}

# Creates an SR. Uses the DRIVER_TYPE global variable.
# XXX Some arguments may be found in multiple positions.
# Arguments:
# 1: SR type
# 2: content-type
# 3: device-config-device
# 4: device-config-server
# 5: device-config-serverpath
# 6: the new host iSCSI IQN
# 7: device-config-target
# 8: device-config-targetIQN
# 9: device-config-SCSIid
# 10: device-config-target
# 11: device-config-username
# 12: device-config-password
# 13: device-config-aggregate
# 14: device-config-FlexVols
# 17: device-config-username
# 18: device-config-password
# 19: device-config-storagepool
sm_SRCreate()
{
    subject="Create SR (type = $1)"
    debug_test "$subject"
    if [ $DRIVER_TYPE = "nfs" ] ; then
        cmd="$REM_CMD $CMD sr-create name-label='TestSR' physical-size=0 type=\"$1\" content-type=\"$2\" device-config-server=\"$4\" device-config-serverpath=\"$5\""
    elif [[ $DRIVER_TYPE = "lvmoiscsi" || $DRIVER_TYPE = "extoiscsi" ]] ; then
        # FIXME: Need to pick the right host UUID
        set_iscsiIQN ${POOL_HOSTS[0]} "${6}"
        if [ $? -ne 0 ]; then
            return 1
        fi

        if [ ${USE_CHAP} -ne 1 ] ; then
            cmd="$REM_CMD $CMD sr-create name-label='TestSR' physical-size=0 type=\"$1\" content-type=\"$2\" device-config-target=\"$7\" device-config-targetIQN=\"$8\" device-config-SCSIid=\"$9\""
        else
            cmd="$REM_CMD $CMD sr-create name-label='TestSR' physical-size=0 type=\"$1\" content-type=\"$2\" device-config-target=\"$7\" device-config-targetIQN=\"$8\" device-config-SCSIid=\"$9\" device-config-chapuser=\"${CHAP_USERNAME}\" device-config-chappassword=\"${CHAP_PASSWORD}\"" 
        fi
    elif [ $DRIVER_TYPE = "netapp" ] ; then
        # FIXME: Need to pick the right host UUID
        set_iscsiIQN ${POOL_HOSTS[0]} "${6}"
        if [ $? -ne 0 ]; then
            return 1
        fi

        if [ -z ${ALLOCATE} ] ; then
            ALLOCATE="thick"
        fi

	if [ ${USE_CHAP} -ne 1 ]; then
	    cmd="$REM_CMD $CMD sr-create name-label='NetappTestSR' physical-size=\"${15}\" type=\"$1\" content-type=\"$2\" device-config-target=\"${10}\" device-config-username=\"${11}\" device-config-password=\"${12}\" device-config-aggregate=\"${13}\" device-config-FlexVols=\"${14}\" device-config-allocation=\"${ALLOCATE}\" shared=true"
	else
	    cmd="$REM_CMD $CMD sr-create name-label='NetappTestSR' physical-size=\"${15}\" type=\"$1\" content-type=\"$2\" device-config-target=\"${10}\" device-config-username=\"${11}\" device-config-password=\"${12}\" device-config-aggregate=\"${13}\" device-config-FlexVols=\"${14}\" device-config-chapuser=\"${CHAP_USERNAME}\" device-config-chappassword=\"${CHAP_PASSWORD}\" device-config-allocation=\"${ALLOCATE}\" shared=true"
	fi
    elif [ $DRIVER_TYPE = "equal" ] ; then
        # FIXME: Need to pick the right host UUID
        set_iscsiIQN ${POOL_HOSTS[0]} "${6}"
        if [ $? -ne 0 ]; then
            return 1
        fi
	cmd="$REM_CMD $CMD sr-create name-label='EqualTestSR' type=\"$1\" content-type=\"$2\" device-config-target=\"${16}\" device-config-username=\"${17}\" device-config-password=\"${18}\" device-config-storagepool=\"${19}\" shared=true"
    elif [ $DRIVER_TYPE = "lvmohba" ] ; then
	cmd="$REM_CMD $CMD sr-create name-label='TestSR' physical-size=0 type=\"$1\" content-type=\"$2\" device-config-device=\"$3\" shared=true"
    else 
        cmd="$REM_CMD $CMD sr-create name-label='TestSR' physical-size=0 type=\"$1\" content-type=\"$2\" device-config-device=\"$3\""
    fi

    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi    
    return
}

#Args: SR_UUID
SR_unplug()
{
    subject="Unplug PBD"

    #unplug all pbds

    cmd="$REM_CMD $CMD pbd-list sr-uuid=$1 --minimal"
    run $cmd
    PBD_UUIDS=$RUN_OUTPUT

    for PBD in `echo $PBD_UUIDS | sed 's/,/\n/g'` ; do
        debug_test "$subject"
        cmd="$REM_CMD $CMD pbd-unplug uuid=$PBD"
        run $cmd
        if [ $RUN_RC -gt 0 ]; then
            debug_result 1
            incr_exitval
            return 1
        else
            debug_result 0
        fi
    done
}

# Deletes an SR.
# Arguments:
# 1: the UUID of the SR to delete
sm_SRDelete()
{
    subject="Delete SR"
    debug_test "$subject"

    #first unplug all pbds

    cmd="$REM_CMD $CMD pbd-list sr-uuid=$1 --minimal"
    run $cmd
    PBD_UUIDS=$RUN_OUTPUT

    for PBD in `echo $PBD_UUIDS | sed 's/,/\n/g'` ; do
        cmd="$REM_CMD $CMD pbd-unplug uuid=$PBD"
        run $cmd
    done

    cmd="$REM_CMD $CMD sr-destroy uuid=$1"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi    
    return
}

#Args: UUID, "Device String"
sm_SRAttach()
{
    subject="Attach SR"
    debug_test "$subject"
    cmd="$REM_CMD $CMD sr_attach -t${SUBSTRATE_TYPE} -d$2 $1"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi  
}

#Args: UUID, "Device String"
sm_SRDetach()
{
    subject="Detach SR"
    debug_test "$subject"
    cmd="$REM_CMD $CMD sr_detach -t${SUBSTRATE_TYPE} -d$2 $1"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi    
}

# Retrieves the parameters of the specified SR. The parameters are stored in the
# RUN_OUTPUT global variable.
# Arguments:
# 1: the UUID of the SR
sm_SRGetParams()
{
    subject="
SM GetParams
============
"
    debug "$subject"
    cmd="$REM_CMD $CMD sr-param-list uuid=$1"
    run $cmd
    debug "$RUN_OUTPUT"
}

# Retrieves the physical size of an SR.
# Arguments:
# 1: the UUID of the SR
sm_SRGetPhysicalSize()
{
    subject=" SR Get Physical Size"
    debug_test "$subject"
    cmd="$REM_CMD $CMD sr-list uuid=$1 params=physical-size --minimal"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi    
}


# Creates a VDI.
# Arguments:
# 1: The UUID of the SR where the VDI is to be created.
# 2:
# 3: name-label
# 4: size
sm_VDIAdd()
{
    subject="Add VDI"
    debug_test "$subject"
    if [ ! -z ${VMHINT} ] ; then
	cmd="$REM_CMD $CMD vdi-create sr-uuid=$1 name-label=$3 type=\"user\" \
        virtual-size=$4 sm-config-vmhint=$VMHINT"
    else
	cmd="$REM_CMD $CMD vdi-create sr-uuid=$1 name-label=$3 type=\"user\" \
        virtual-size=$4"
    fi
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

# Snapshots a VDI.
# 1: the UUID of the VDI to snapshot
# 2: driver-params-epochhint
sm_VDISnap()
{
    subject="Snapshot VDI"
    debug_test "$subject"
    if [ $DRIVER_TYPE = "netapp" ] ; then
        cmd="$REM_CMD $CMD vdi-snapshot uuid=$1 driver-params-epochhint=$2"
    else
        cmd="$REM_CMD $CMD vdi-snapshot uuid=$1"
    fi
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

#Args: UUID, "Device String", VDI_UUID
sm_VDIAttach()
{
    subject="Attach VDI"
    debug_test "$subject"
    cmd="$REM_CMD $CMD vdi_attach -t${SUBSTRATE_TYPE} -d$2 $1 $3"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

#Args: UUID, "Device String", VDI_UUID
sm_VDIDetach()
{
    subject="Detach VDI"
    debug_test "$subject"
    cmd="$REM_CMD $CMD vdi_detach -t${SUBSTRATE_TYPE} -d$2 $1 $3"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

# Deletes a VDI.
# Arguments:
# 1: TODO
# 2: TODO
# 3: the UUID of the VDI to be destroyed
sm_VDIDelete()
{
    subject="Delete VDI"
    debug_test "$subject"
    cmd="$REM_CMD $CMD vdi-destroy uuid=$3"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

# Creates a VBD.
# Arguments:
# 1: the UUID of the VDI
# 2: the UUID of the VM
# 3: a device number, as indicated by allowed-VBD-devices.
# 4: RO/RW (read-only/read-write)
sm_VBDCreate()
{
    subject="Create VBD"
    debug_test "$subject"
    cmd="$REM_CMD $CMD vbd-create vdi-uuid=$1 vm-uuid=$2 device=$3 mode=$4"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

# Retrieves the device that corresponds to the specified VBD UUID.
# Arguments:
# 1: the UUID of the VBD
sm_VBDGetDevice()
{
    subject="Get VBD device"
    debug_test "$subject"
    cmd="$REM_CMD $CMD vbd-param-get uuid=$1 param-name=device"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

# Destroys the specified VBD.
# Arguments:
# 1: UUID of the VBD to destroy
sm_VBDDelete()
{
    subject="Delete VBD"
    debug_test "$subject"
    cmd="$REM_CMD $CMD vbd-destroy uuid=$1"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

# Plugs a VBD.
# Arguments:
# 1: UUID of the VBD to plug
sm_VBDPlug()
{
    subject="Plugging VBD"
    debug_test "$subject"


    cmd="$REM_CMD $CMD vbd-plug uuid=$1"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        #sleep 5
        return 0
    fi
}

# Unplugs a VBD.
# Arguments:
# 1: the UUID of the VBD to unplug
# 2: an optional timeout, expressed in seconds
sm_VBDUnplug()
{
    subject="Unplugging VBD"
    debug_test "$subject"

    if [ -z "$2" ] ; then
        TIMEOUT_FLAG=" timeout=30"
    else
        # FIXME shouldn't this be $2?
        TIMEOUT_FLAG=" timeout=$1"
    fi

    cmd="$REM_CMD $CMD vbd-unplug uuid=$1 ${TIMEOUT_FLAG}"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

sm_VMGetMAC()
{
    subject="Get VM MAC address"
    debug_test "$subject"
    cmd="$REM_CMD $CMD vbd-param-get uuid=$1 param-name=device"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

sm_getprimaryVMVDI()
{
    subject="Querying primary VBD"
    debug_test "$subject"
    cmd="$REM_CMD $CMD vbd-list bootable=true vm-uuid=$1 --minimal | cut -d',' -f 1"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi

    cmd="$REM_CMD $CMD vbd-param-get uuid=$RUN_OUTPUT param-name=vdi-uuid"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        echo "Primary VDI succeeded: ${GLOBAL_RET}"
        return 0
    fi
}



#Args: UUID, "Device String", VDI_UUID
sm_VDIResize()
{
    subject="Extend VDI"
    debug_test "$subject"
    cmd="$REM_CMD $CMD vdi-resize uuid=$2 disk-size=$3"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

#Args: UUID, "Device String", VDI_UUID
sm_VDIClone()
{
    subject="Clone VDI"
    debug_test "$subject"
    cmd="$REM_CMD sm vdi_clone -t${SUBSTRATE_TYPE} -d$2 $1 $3"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

#Args: UUID, "Device String", VDI_UUID
sm_VDISnapshot()
{
    subject="Snapshot VDI"
    debug_test "$subject"
    cmd="$REM_CMD sm vdi_snapshot -t${SUBSTRATE_TYPE} -d$2 $1 $3"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

# Retrieves the parameters of a VDI. The retrieved parameters are stored in the
# RUN_OUPUT global variable.
# 1: TODO
# 2: the UUID of the VDI
sm_VDIGetParams()
{
    subject="
SM VdiGetParams
===============
"
    debug "$subject"
    cmd="$REM_CMD $CMD vdi-param-list uuid=$2"
    run $cmd
    debug "$RUN_OUTPUT"
}

#Args: BLOCK_DEVICE_STRING
sm_CheckPbdUnused()
{
    INUSE=0
    subject="Checking PBD Unused"
    debug_test "$subject"

    #first, get list of PBDs
    cmd="$REM_CMD $CMD pbd-list --minimal"
    run $cmd

    #then per PBD, check to see if device matches argument
    for PBD in `echo $RUN_OUTPUT | sed 's/,/\n/g'`; do
        cmd="$REM_CMD $CMD pbd-list uuid=$PBD params=device-config --minimal | cut -d' ' -f2"
        run $cmd
        if [ "$RUN_OUTPUT" = "$1" ] ; then
            INUSE=1
            # we have a match. lets print the SR info
            cmd="$REM_CMD $CMD pbd-list uuid=$PBD params=sr-uuid --minimal"
            run $cmd

            cmd="$REM_CMD $CMD sr-list uuid=$RUN_OUTPUT"
            run $cmd

            SR_INFO=$RUN_OUTPUT
            break
        fi
    done

    if [ $INUSE -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}



############################################################
## SR Driver Operational Tasks
############################################################

# Creates an SR and verifies it.
#Args: type, content-type and "Device String"
#Sets: SR_ID
smCreate()
{
    sm_SRCreate "${1}" "${2}" "${3}" "${4}" "${5}" "${6}" "${7}" \
                "${8}" "${9}" "${10}" "${11}" "${12}" "${13}" "${14}" "${15}" \
                "${16}" "${17}" "${18}" "${19}"
    SR_ID=${GLOBAL_RET}
    ${DRIVER_TYPE}_verify_smCreate ${SR_ID}
    return ${EXITVAL}
}

# Deletes an SR.
# Arguments:
# 1: the UUID of the SR to delete
# 2: XXX
#Args: SRID, "Device String"
smDelete()
{
    sm_SRDelete $1 $2
    DEV=`echo "$2" | sed 's/.*@//g'`
    ${DRIVER_TYPE}_verify_smDelete $1 $DEV
}

#Args: SRID, "Device String"
smAttach()
{
    sm_SRAttach $1 $2
    return ${EXITVAL}
}

#Args: SRID
smDetach()
{
    sm_SRDetach $1 $2
    return ${EXITVAL}
}

# Adds a VDI and then verifies it. Sets the VDI UUID in the VDI_ID global
# variable.
#
# Arguments:
# 1: The UUID of the SR where the VDI is to be created.
# 2: TODO
# 3: name-label
# 4: size
smAddVdi()
{
    sm_VDIAdd $1 $2 $3 $4
    DEV=`echo "$2" | sed 's/.*@//g'`
    VDI_ID=${GLOBAL_RET}
    ${DRIVER_TYPE}_verify_smAddVdi $1 ${VDI_ADD_UUID}
    return ${EXITVAL}
}

# Snapshots a VDI. Stores the UUID of the new VDI that corresponds to the snapshot in VDI_ID.
# Arguments:
# 1: the UUID of the VDI to snapshot
# 2: driver-params-epochhint
smVDISnap()
{
    sm_VDISnap $1 $2
    VDI_ID=${GLOBAL_RET}
    return ${EXITVAL}
}

#Args: SRID, VDI_ID
smAttachVdi()
{
    sm_VDIAttach $1 $2 $3
    DEV=`echo "$2" | sed 's/.*@//g'`
    ${DRIVER_TYPE}_verify_smAttachVdi $1 $DEV $3
    return ${EXITVAL}
}

#Args: SRID, VDI_ID
smDetachVdi()
{
    sm_VDIDetach $1 $2 $3
    DEV=`echo "$2" | sed 's/.*@//g'`
    ${DRIVER_TYPE}_verify_smDetachVdi $1 $DEV $3
    return ${EXITVAL}
}

#Args: SRID, VDI_ID, Size
smResizeVdi()
{
    sm_VDIResize $1 $2 $3
    ${DRIVER_TYPE}_verify_smExtendVdi $1 $2 $3
    return ${EXITVAL}
}

#Args: SRC_SRID, VDI_ID, DST_SRID
smCopyVdi()
{
    sm_VDICopy $1 $2 $3
    DEV=`echo "$2" | sed 's/.*@//g'`
    ${DRIVER_TYPE}_verify_smCopyVdi $1 $DEV $3
    return ${EXITVAL}
}

# Deletes a VDI.
# Arguments:
# 1: TODO
# 2: TODO
# 3: the UUID of the VDI to be destroyed
smDeleteVdi()
{
    sm_VDIDelete $1 $2 $3
    DEV=`echo "$2" | sed 's/.*@//g'`
    ${DRIVER_TYPE}_verify_smDeleteVdi $1 $DEV $3
    return ${EXITVAL}
}

# Creates a VBD.
# Arguments:
# 1: the UUID of the VDI
# 2: the UUID of the VM
# 3: a device number, as indicated by allowed-VBD-devices.
smCreateVbd()
{
    sm_VBDCreate $1 $2 $3 "rw"
    VBD_ID=${GLOBAL_RET}
    return ${EXITVAL}
}

# Create a read-only VBD. Stores the UUID of the newly-created VBD in VBD_ID.
# Arguments:
# 1: the UUID of the VDI
# 2: the UUID of the VM
# 3: a device number, as indicated by allowed-VBD-devices. 
#Args: VDI_ID, VM_ID, device
smCreateROVbd()
{
    sm_VBDCreate $1 $2 $3 "ro"
    VBD_ID=${GLOBAL_RET}
    return ${EXITVAL}
}

# Retrieves the device that corresponds to the specified VBD UUID and stores in
# the VBD_DEVICE global variable.
# Arguments:
# 1: the UUID of the VBD
smGetVbdDevice()
{
    sm_VBDGetDevice $1
    VBD_DEVICE=${GLOBAL_RET}
    return ${EXITVAL}
}

# Destroys the specified VBD.
# Arguments:
# 1: UUID of the VBD to destroy
smDeleteVbd()
{
    sm_VBDDelete $1
    return ${EXITVAL}
}

# Plugs a VBD.
# Arguments:
# 1: UUID of the VBD to plug
smPlugVbd()
{
    sm_VBDPlug $1
    return ${EXITVAL}
}

# Unplugs a VBD.
# Arguments:
# 1: the UUID of the VBD to unplug
# 2: an optional timeout, expressed in seconds
smUnplugVbd()
{
    sm_VBDUnplug $1 $2
    return ${EXITVAL}
}

getprimaryVMVDI()
{
    sm_getprimaryVMVDI $1
    return ${EXITVAL}
}

#Args: VM_ID
smGetVMMAC()
{
    sm_VMGetMAC $1
    return ${EXITVAL}
}

# Retrieves the SR parameters (stored in the RUN_OUTPUT global variable).
# Arguments:
# 1: the UUID of the SR
smGetParams()
{
    sm_SRGetParams $1 
    return ${EXITVAL}
}

# Retrieves the physical size of an SR.
# Arguments:
# 1: the UUID of the SR
smGetPhysicalSize()
{
    sm_SRGetPhysicalSize $1 
    return ${EXITVAL}
}


# Retrieves the parameters a VDI. The retrieved parameters are stored in the
# RUN_OUTPUT global variable.
# Arguments:
# 1: TODO
# 2: the UUID of the VDI
smVdiGetParams()
{
    sm_VDIGetParams $1 $2
}

#Args: BLOCK_DEVICE_STRING
smCheckPbdUnused()
{
    sm_CheckPbdUnused $1
    return ${EXITVAL}
}

_delete_vm()
{
    cmd="$REM_CMD $CMD vm-list name-label=\"$1\" --minimal"
    VMUUID=`$cmd`

    if [ $? -ne 0 ] ; then CLEANUP_RC=`expr ${CLEANUP_RC} + 1` ; fi

    #echo "Found VM with name $1 - UUID: ${VMUUID}"

    cmd="$REM_CMD $CMD vm-uninstall uuid=${VMUUID} --force"
    output=`$cmd`
    if [ $? -ne 0 ] ; then CLEANUP_RC=`expr ${CLEANUP_RC} + 1` ; fi
}

_delete_all_vms()
{
    cmd="$REM_CMD $CMD vm-list is-control-domain=false params=name-label --minimal"
    VMIDS=`$cmd`
    if [ $? -ne 0 ] ; then CLEANUP_RC=`expr ${CLEANUP_RC} + 1` ; fi

    for VM in `echo $VMIDS | sed 's/,/\n/g'` ; do
        #echo "VM = $VM"
        cmd="$REM_CMD $CMD vm-shutdown name-label="$VM" --multiple force=true"
        output=`$cmd`

        _delete_vm "$VM"
    done
}

_unplug_vbds()
{
    cmd="$REM_CMD $CMD vdi-list sr-uuid=\"$1\" --minimal"
    VDIIDS=`$cmd`
    #echo "VDIIDS = $VDIIDS"
    if [ $? -ne 0 ] ; then CLEANUP_RC=`expr ${CLEANUP_RC} + 1` ; fi

    for VDI in `echo $VDIIDS | sed 's/,/\n/g'` ; do
        cmd="$REM_CMD $CMD vbd-list vdi-uuid="$VDI"  --minimal"
        VBDID=`$cmd`
        #echo "VBDID = $VBDID"
        cmd="$REM_CMD $CMD vbd-unplug uuid=\"${VBDID}\""
        output=`$cmd`
        #echo $output
    done
}
_cleanup_sr()
{
    cmd="$REM_CMD $CMD pbd-list sr-uuid=\"$1\" --minimal"
    PBDID=`$cmd`
    if [ $? -ne 0 ] ; then CLEANUP_RC=`expr ${CLEANUP_RC} + 1` ; fi

    #echo "Found PBDID = ${PBDID}"

    cmd="$REM_CMD $CMD pbd-unplug uuid=\"${PBDID}\""
    output=`$cmd`
    if [ $? -ne 0 ] ; then CLEANUP_RC=`expr ${CLEANUP_RC} + 1` ; fi
    #echo "Unplugged ${PDBID}"

    cmd="$REM_CMD $CMD sr-destroy uuid=$1"
    output=`$cmd`
    if [ $? -ne 0 ] ; then CLEANUP_RC=`expr ${CLEANUP_RC} + 1` ; fi
    #echo "Destroyed ${SRID}"
}

#Cleanup VMs on host
#Arg1 SRID
host_cleanup()
{
    CLEANUP_RC=0

    # A bit of skanky calls to force shutdown all the VMs,
    # delete their VDIs, delete the VIFs, delete the VM itself
    # and then unplug the PBD and destroy the SR.
    #
    # No error checking done here. 

    if [ -z $NOCLEANUP ] ; then
        debug && debug "Cleaning up host..."
        set +e 

        _delete_all_vms

        _unplug_vbds ${SR_ID}

        _cleanup_sr ${SR_ID}

        if [ $CLEANUP_RC -ne 0 ] ; then
            debug "Some cleanup failed ($CLEANUP_RC). Manual cleanup required"
            exit 1
        else
            debug "Done."
        fi
    else
        debug && debug "Skipping cleaning up host..."
    fi
}

#Sets DOM0_ID
smGetDom0ID()
{
    if [ ! -z $DOM0_ID ] ; then
        # doesn't change
        return
    fi

    cmd="$REM_CMD $CMD vm-list is-control-domain=true --minimal | cut -d',' -f1"
    DOM0_ID=`$cmd`
    return $RUN_RC
}

#Sets POOL_ID

smGetPoolID()
{
    cmd="$REM_CMD $CMD pool-list --minimal"
    run $cmd
    POOL_ID="$RUN_OUTPUT"

    if [ "z$POOL_ID" = "z" ] ; then
        return 1
    fi
    
    return $RUN_RC
}

# Retrieves default SR. Stores in SR_DEFAULT_ID.
# Arguments: none.
smGetDefaultSR()
{
    subject="Getting default SR"
    debug_test "$subject"

    smGetPoolID

    if [ $? -ne 0 ] ; then
        debug_result 1
        debug "ERROR: Failed to get pool ID"
        incr_exitval
        return 1
    fi

    cmd="$REM_CMD $CMD pool-param-get uuid=${POOL_ID} param-name=default-SR"
    run $cmd
    debug_result 0
    if [ "${RUN_OUTPUT}" = "<not in database>" ]; then
        SR_DEFAULT_ID="None"
    else
        SR_DEFAULT_ID=${RUN_OUTPUT}
    fi
    return $RUN_RC
}

#sets default SR to SR_ID
smSetDefaultSR()
{
    subject="Setting default SR"
    debug_test "$subject"

    smGetPoolID

    if [ $? -ne 0 ] ; then
        debug_result 1
        debug "ERROR: Failed to get pool ID"
        incr_exitval
        return 1
    fi

    if [ "$1" = "None" ]; then
        cmd="$REM_CMD $CMD pool-param-clear uuid=${POOL_ID} param-name=default-SR="
    else
        cmd="$REM_CMD $CMD pool-param-set uuid=${POOL_ID} default-SR=$1"
    fi
    run $cmd
    debug_result 0
    return $RUN_RC
}

# Tells whether an SR with the specified UUID exists.
smCheckSR()
{
    subject="Checking SR exists"
    debug_test "$subject"

    cmd="$REM_CMD $CMD sr-param-get uuid=$1 param-name=type"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        SR_DRIVER=`echo ${RUN_OUTPUT} | sed 's/ext/file/'`
        return 0
    fi
}

# Uninstalls a virtuam machine.
# Arguments:
# 1: UUID of the SR (needed for deleting the VDIs)
# 2: UUID of the VM to delete
smUninstallVM()
{
    #First remove the VDI's
    VDIUUIDS=`$REM_CMD $CMD vbd-list vm-uuid="$2" params=vdi-uuid --minimal`
    for vdi in `echo ${VDIUUIDS} | sed 's/,/\n/g'` ; do 
        smDeleteVdi $1 "bogus" ${vdi}
    done

    #And the VIF's
    VIFUUIDS=`$REM_CMD $CMD vif-list vm-uuid="$2" --minimal`
    for vif in `echo ${VIFUUIDS} | sed 's/,/\n/g'` ; do 
        smDeleteVif ${vif}
    done


    subject="Deleting VM"
    debug_test "$subject"

    cmd="$REM_CMD $CMD vm-uninstall uuid=$2 --force"

    run $cmd

    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

# Clones a virtual machine.
# TODO document arguments
# Arg 1 -> VMID
# Arg 2 -> New Name Label
cloneVM()
{
    subject="Clone VM"
    debug_test "$subject"
    cmd="$REM_CMD $CMD vm-clone uuid=$1 new-name-label=\"$2\""
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        GLOBAL_RET=$RUN_OUTPUT
        debug_result 0
        return 0
    fi
}

# Starts a virtual machine.
# TODO document arguments
# Arg 1 -> VMID
start_vm()
{
    subject="Starting VM"
    debug_test "$subject"
    ADDON=""
    if [ ! -z $ONHOST ]; then
        ADDON="on=$ONHOST"
    fi
    cmd="$REM_CMD $CMD vm-start $ARGS uuid=$1 $ADDON"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi
}

# Retrieves the IP address of a vitual machine.
# Arg 1 -> VMID
# Arg 2 -> Quiet mode
getVMIP()
{
    subject="Getting VM IP"
    if [ $2 -ne 1 ] ; then
        debug_test "$subject"
    fi
    cmd="$REM_CMD $CMD vm-list params=networks uuid=$1 --minimal | cut -d' ' -f2 | tr ';' ' '"
    run $cmd

    if [ $RUN_RC -gt 0 ]; then
        if [ $2 -ne 1 ] ; then
            debug_result 1
        fi
        RUN_OUTPUT=$MYRUN_OUTPUT
        incr_exitval
        return 1
    else
        if [ $2 -ne 1 ] ; then
            debug_result 0
        fi

        echo $RUN_OUTPUT | grep -q -E "[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" &> /dev/null
        if [ $? -gt 0 ] ; then
            #not set yet
            return 1
        fi
        
        #TODO : this might break with multiple vifs that have IP addresses?
        GLOBAL_RET=$RUN_OUTPUT
        sleep $GUEST_IP_SLEEP_DELAY
        return 0
    fi
}

vm_get_num_vifs()
{
    subject="Determine number of Vifs"
    debug_test "$subject"
    # todo - get rid of grep
    cmd="$REM_CMD $CMD vif-list vm-uuid=$1 | grep ^uuid  |  wc -l"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

# Verify boot
# Arg 1 -> VMI
verify_boot()
{
    return 0
}

# Starts a virtual machine and waits for it to completely boot.
# TODO document arguments.
# Arg 1 -> VMID
smBootVM()
{
    EXITVAL=0
    start_vm $1

    if [ $EXITVAL -eq 0 ] ; then
        if [ $TEMPLATE_ALIAS = "debian" ] ; then
            #sleep 10 minutes for debian
            WAIT_LOOPS=120
            SLEEP_TIME=5
        else
            #sleep 90 minutes for windows
            WAIT_LOOPS=90
            SLEEP_TIME=60
        fi

        if [ $? == 0 ]; then
            #Sleep 30 secs to allow vm to boot
            #TODO make this check really look at the guest status. Like IP address present?
            vm_get_num_vifs $1
            NUM_VIFS=$GLOBAL_RET

            if [ $NUM_VIFS -gt 0 ] ; then
                # try for 2 minutes to get IP address
                for i in `seq 1 $WAIT_LOOPS` ;  do
                    getVMIP $1 1
                    if [ $? -eq 0 ] ; then
                        NUM_SECS=$((i*5))
                        IP_ADDR="${GLOBAL_RET}"
                        debug "Found VM IP $IP_ADDR after $NUM_SECS seconds"
                        break
                    fi
                    sleep $SLEEP_TIME
                done

                if [ -z "$IP_ADDR" ] ; then
                    debug "Did not find an IP address"
                    EXITVAL=1
                fi
            fi
            verify_boot $1
        fi
    fi
    return ${EXITVAL}
}

# Stops a virtual machine.
# Arg 1 -> VMID
smStopVM()
{
    subject="Stopping VM"
    debug_test "$subject"
    cmd="$REM_CMD $CMD vm-shutdown uuid=$1"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi
}

#Args: UUID
sm_VIFDelete()
{
    subject="Delete VIF"
    debug_test "$subject"
    cmd="$REM_CMD $CMD vif-destroy uuid=$1"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}


#Args: VIF_ID
smDeleteVif()
{
    sm_VIFDelete $1
    return ${EXITVAL}
}

# Creates a symbolic link between the LVMSR Python file the supplied file name,
# effectively creating a "new" type of SR.
# Arguments:
# 1: The name of the "new" SR to create.
copy_lvm_plugin()
{
    DUMMY_SR_TYPE=$1
    subject="Creating dummy SR type (${DUMMY_SR_TYPE})"
    debug_test "$subject"
    cmd="$REM_CMD ln -s /opt/xensource/sm/LVMSR /opt/xensource/sm/${DUMMY_SR_TYPE}SR &> /dev/null"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 2
        #incr_exitval
        return 2
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT
        return 0
    fi
}

# Restarts xapi so it can pick up new storage types.
restart_xapi()
{
    subject="Restarting xapi to pick up new storage type"

    debug_test "$subject"
    cmd="$REM_CMD service xapi restart &> /dev/null"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=$RUN_OUTPUT

        #give xapi a bit of time to settle
        sleep 5

        return 0
    fi
}


# Sets up a target machine for passwordless SSH logins. (TODO verify)
# $1 host
# $2 public certificate file
# $3 host password

install_ssh_certificate() {
    PUB_CERT_PATH="$2.pub"
    if [ -z $3 ] ; then
        debug "Please pass in a root password for $1"
        debug_result 1
        incr_exitval
        return 1
    fi

    if [ ! -f ${PUB_CERT_PATH} ] ; then
        debug "Please pass in the path to a valid public SSH certificate"
        debug_result 1
        incr_exitval
        return 1
    fi


    subject="Installing SSH certificate"
    debug_test "$subject"

    FULL_KEY="`cat ${PUB_CERT_PATH} | head -n1 `"
    KEY=`expr substr ${FULL_KEY:8:100} 1 99`

    CARBON_DOMU_PASSWORD=$3
    ssh_password_wrapper ssh "root@$1" "mkdir -p /root/.ssh" &> /dev/null

    if [ $? -ne 0 ] ; then
        debug "Failed to mkdir"
        debug "doing it again to grab command output"
        debug "cmd = ssh_password_wrapper ssh root@$1 mkdir -p /root/.ssh"
        ssh_password_wrapper ssh "root@$1" "mkdir -p /root/.ssh" 
        debug_result 1
        incr_exitval
        return 1
    fi

    ssh_password_wrapper scp "${PUB_CERT_PATH}" "root@$1:/root/.ssh/pubkey" &> /dev/null

    if [ $? -ne 0 ] ; then
        debug "Failed to scp pubkey"
        debug_result 1
        incr_exitval
        return 1
    fi

    ssh_password_wrapper ssh "root@$1" "if ! grep -q \"`cut -d ' ' -f 2 ${PUB_CERT_PATH}`\" /root/.ssh/authorized_keys \; then cat /root/.ssh/pubkey >> /root/.ssh/authorized_keys \; fi" &> /dev/null

    if [ $? -ne 0 ] ; then
        debug "Failed to add pubkey to auth keys"
        debug_result 1
        incr_exitval
        return 1
    fi

    debug_result 0
}

# $1 host
install_scsiID_helper() {
    subject="Installing SCSIid helper"
    debug_test "$subject"
    UPLOAD="/tmp/`date +%s-%N`"

    echo "import sys" >> $UPLOAD
    echo "sys.path.insert(0, '/opt/xensource/sm')" >> $UPLOAD
    echo "import scsiutil" >> $UPLOAD
    echo "" >> $UPLOAD
    echo "try:" >> $UPLOAD
    echo "    SCSIid = scsiutil.getSCSIid(sys.argv[1])" >> $UPLOAD
    echo "    print SCSIid" >> $UPLOAD
    echo "except:" >> $UPLOAD
    echo "    sys.exit(1)" >> $UPLOAD

    cmd="chmod +x $UPLOAD"
    run $cmd

    cmd="/usr/bin/scp -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null -i ${SSH_PRIVATE_KEY} $UPLOAD root@$1:/etc/xensource/SCSIutil.smrt"
    run $cmd
    if [ $RUN_RC -ne 0 ]; then
	debug_result 1
	incr_exitval
    else
	debug_result 0
    fi
    test_exit 1
    cmd="rm -f $UPLOAD"
    run $cmd
}

# Receives a list of "variable=value" pairs and for each pair it checks wheher
# the supplied variable is an expected one, and if so it initializes a BASH
# variable with the same name to the indicated value.
process_arguments() {
    until [ -z "$1" ]
    do
      process_args $1
      shift
    done
}

# Ensures that some necessary arguments have been supplied.
check_req_args() {
    if [ -z ${DEVSTRING} ]; then
        debug "Must supply a device string"
        usage
        exit 1
    fi

    if [ -z ${REMHOSTNAME} ]; then
        debug "Must supply a remote host IP or hostname"
        usage
        exit 1
    fi

    if [ -z ${PASSWD} ] ; then
        debug "Must supply a root password for ${REMHOSTNAME}"
        usage
        exit 1
    fi
}

# Ensures that some necessary S/W has been installed.
check_req_sw() {
    if [ ! -f /usr/bin/ploticus ] ; then
        debug "Ploticus not installed, performance graphs disabled."

    fi

    if [ ! -f /usr/bin/expect ] ; then
        debug "Please install the expect package"
        exit 1
    fi
}

check_req_fc_hw() {
    FC_STRING="Fibre Channel"

    cmd="$REM_CMD grep '${FC_STRING}' /etc/sysconfig/hwconf >& /dev/null"
    out=`$cmd`
    fc=$?

    if [ $fc -eq 0 ]; then
	return 0
    else
	return 1
    fi 
}

#TODO : get XE versoin, as well as blktap version
print_version_info() {
    debug "
Test Version Information: ${VERSION}
=============
"

    smGetDom0ID
    START_DATE=`date`
    debug "Dom0 HostName = ${REMHOSTNAME}
Dom0 ID = ${DOM0_ID}

Test started: ${START_DATE}
"

}

print_exit_info() {
    END_DATE=`date`
    debug "

Test finished: ${END_DATE}
"
}


