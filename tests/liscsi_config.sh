#!/bin/bash
## This is the liscsi-specific configuration script
## to enable setup of liscsi-backed SRs
## during automated testing

if [ -z ${MY_HOSTNAME} ]; then
    MY_HOSTNAME=`hostname`
fi

if [ -z ${ISCSIADM} ]; then
    ISCSIADM="/sbin/iscsiadm"
fi

if [ -z ${NUM_LUNS} ]; then
    NUM_LUNS=1
fi

if [ -z ${INITIATORNAME_FILE} ]; then
    INITIATORNAME_FILE="/etc/iscsi/initiatorname.iscsi"
fi

# Initializes the iSCSI initiator file.
# Arguments: none.
iqn_initialise()
{
    subject="Setup IQN identifier"
    debug_test "$subject"
    cmd="$REM_CMD echo "InitiatorName=${IQN_INITIATOR_ID}" > $INITIATORNAME_FILE"
    `$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi

    cmd="$REM_CMD echo "InitiatorAlias=${MY_HOSTNAME}" >> $INITIATORNAME_FILE"
    `$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi
}

# Starts the Open-iSCSI service.
# Arguments: none.
open_iscsi_start()
{
    subject="Starting open-iscsi"
    debug_test "$subject"
    cmd="$REM_CMD /sbin/service open-iscsi stop >& /dev/null"
    `$cmd`

    cmd="$REM_CMD /sbin/service open-iscsi start >& /dev/null"
    `$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi
}

# Discovers the iSCSI target.
# Aguments: none.
# TODO Figure out what exactly this functions does. Probably if the target
# exists it sets ISCSI_RECID and ISCSI_RECIQN variables and returns 0, else it
# returns 1.
discover_target()
{
    subject="Discover liscsi target"
    debug_test "$subject"
    cmd="$REM_CMD ${ISCSIADM} -m discovery -t st -p ${LISCSI_TARGET_IP}"
    output=`$cmd | grep ${LISCSI_TARGET_ID} | grep ${LISCSI_TARGET_IP}`
    ret=$?
    if [ $ret -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        ISCSI_RECID=`echo $output | cut -d" " -f1`
        ISCSI_RECIQN=`echo $output | cut -d" " -f2`
        debug "Iscsi target record is: ${ISCSI_RECID} and ${ISCSI_RECIQN}"
        return 0
    fi
}

# Attaches an iSCSI target. If the target is already attached, the function
# succesfully returns.
#Args: recid, reciqn
# TODO This function uses some global variables, document this and how they are
# used.
# TODO This function sleeps in order to allow the devices to appear. Improve
# this by checking /dev.
attach_target()
{
    subject="Attach liscsi target"
    debug_test "$subject"

    #Verify whether target already attached
    GLOBAL_DEBUG=0
    verify_device
    ret=$?
    GLOBAL_DEBUG=1
    if [ $ret == 0 ]; then
        #Already attached
        debug_result 1
        return 0
    fi
    decr_exitval

    cmd="$REM_CMD ${ISCSIADM} -m node -T $2 -p $1 -l >& /dev/null"
    output=`$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        sleep 10
        debug_result 0
        return 0
    fi   
}

# Detaches and deletes an iSCSI target.
#Args: recid, reciqn
detach_target()
{
    subject="Detach liscsi target"
    debug_test "$subject"
    cmd="$REM_CMD ${ISCSIADM} -m node -T $2 -p $1 -u >& /dev/null"
    output=`$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
		debug_result 1
		incr_exitval
		return 1
    else
		debug_result 0
    fi

    subject="Delete liscsi target"
    debug_test "$subject"
    cmd="$REM_CMD ${ISCSIADM} -m node -T $2 -p $1 -o delete >& /dev/null"
    output=`$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
		debug_result 1
		incr_exitval
		return 1
    else
		debug_result 0
		return 0
    fi   
}

# Checks whether the LUNs exist under /dev.
# Arguments: none.
# TODO improve documentation
verify_device()
{
    subject="Verify LUN device path"
    debug_test "$subject"
    GLOBAL_RET=""
    for ((  LUN = 0 ;  LUN < ${NUM_LUNS};  LUN++  ))
    do
        cmd="$REM_CMD ls /dev/iscsi/${LISCSI_TARGET_ID}/LUN${LUN} >& /dev/null"
        output=`$cmd`
        ret=$?
        if [ $ret -gt 0 ]; then
            debug_result 1
            incr_exitval
            GLOBAL_RET=""
            return 1
        else
            GLOBAL_RET="/dev/iscsi/${LISCSI_TARGET_ID}/LUN${LUN} ${GLOBAL_RET}"
        fi
    done
    debug_result 0
    return 0
}

#Args: recid, reciqn
automate_login()
{
    subject="Automate liscsi login"
    debug_test "$subject"
    cmd="$REM_CMD ${ISCSIADM} -m node -T $2 -p $1 --op update -n node.conn[0].startup -v automatic >& /dev/null"
    output=`$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
	debug_result 1
	incr_exitval
	return 1
    fi

    cmd="$REM_CMD ${ISCSIADM} -m node -T $2 -p $1 --op update -n node.startup -v automatic >& /dev/null"
    output=`$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
	debug_result 1
	incr_exitval
	return 1
    else
	debug_result 0
	return 0
    fi 
}

add_runlevel()
{
    subject="Add to runlevel"
    debug_test "$subject"
    cmd="$REM_CMD chkconfig --add open-iscsi >& /dev/null"
    `$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
	debug_result 1
	incr_exitval
	return 1
    fi

    cmd="$REM_CMD chkconfig --level 35 open-iscsi on >& /dev/null"
    `$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
	debug_result 1
	incr_exitval
	return 1
    else
	debug_result 0
	return 0
    fi
}

####Main Function
liscsi_setup()
{
    debug && debug "Initialising iscsi disks"
    iqn_initialise
    if [ $ret -gt 0 ]; then
	debug "iqn_initialise failed"
	return 1
    fi
    open_iscsi_start
    if [ $ret -gt 0 ]; then
	debug "open_iscsi_start failed"
	return 1
    fi

    discover_target
    if [ $ret -gt 0 ]; then
	debug "discover_target failed"
	return 1
    fi

    attach_target ${ISCSI_RECID} ${ISCSI_RECIQN}
    if [ $ret -gt 0 ]; then
	debug "attach_target failed"
	return 1
    fi

    automate_login ${ISCSI_RECID} ${ISCSI_RECIQN}
    if [ $ret -gt 0 ]; then
	debug "automate_login failed"
	return 1
    fi

    add_runlevel
    if [ $ret -gt 0 ]; then
	debug "add_runlevel failed"
	return 1
    fi

    verify_device
    if [ $ret -gt 0 ]; then
	debug "verify_device failed"
	return 1
    else
	LISCSI_DEVICE_STRING=${GLOBAL_RET}
    fi

    return 0
}

liscsi_DEVICE_STRING()
{
    echo ${LISCSI_DEVICE_STRING}
}

liscsi_disconnect()
{
    detach_target ${ISCSI_RECID} ${ISCSI_RECIQN}
    return
}

liscsi_cleanup()
{
    subject="Logout from iscsi targets"
    debug_test "$subject"
    cmd="$REM_CMD ${ISCSIADM} -m node --logoutall=all >& /dev/null"
    output=`$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
	debug_result 0
	return 0
    else
	debug_result 0
    fi

    subject="Removing iscsi records"
    debug "$subject"
    cmd="$REM_CMD ${ISCSIADM} -m node"
    `$cmd > ${LOGFILE}`
    exec < ${LOGFILE}
    while read line
    do
      target=`echo $line | cut -d" " -f2`
      ip=`echo $line | cut -d" " -f1`
      newcmd="$REM_CMD ${ISCSIADM} -m node -T $target -p $ip -o delete >& /dev/null"
      subject="Deleting $target"
      debug_test "$subject"
      out=`$newcmd`
      ret=$?
      if [ $ret -gt 0 ]; then
	  debug_result 1
      else
	  debug_result 0
      fi
    done
    return 0
}

