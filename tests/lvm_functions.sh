#!/bin/bash


############################################################
## Generic LVM helper functions
############################################################

lvm_vdi_count()
{
    subject="Query installed VM disks"
    debug_test "$subject"
    cmd="$REM_CMD lvs --noheadings"
    output=`$cmd | grep $2 | wc -l`
    ret=$output
    if [ "$ret" == 0 ]; then
        debug_result 1
        incr_exitval
        GLOBAL_RET=0
        return 1
    else
        debug_result 0
        GLOBAL_RET=${output}
        return 0
    fi
}

#Downsize disk - Used for debugging speedup
# Arg 1 -> VMID
# Arg 2 -> Size
# Arg 3 -> Name
# Arg 4 -> SRID
lvm_downsize_disk()
{
    if [ ${CLONE_DEBUG} == 0 ]; then
        return 0
    fi

    cmd="$REM_CMD lvs | grep $1.$3 > /dev/null"
    `$cmd`
    if [ $? -gt 0 ]; then
        return 0
    fi

    subject="[CLONE_DEBUG]downsizing disk"
    debug_test "$subject"
    cmd="$REM_CMD lvremove -f /dev/VG_XenStorage-$4/LV-$1.$3 2>&1 | cat > /dev/null"
    `$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
        debug_result 1
        return 1
    fi
        
    cmd="$REM_CMD lvcreate -L$2M -nLV-$1.$3 VG_XenStorage-$4 2>&1 | cat > /dev/null"
    `$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
        debug_result 1
        return 1
    else
        debug_result 0
    fi
    return 0
}

lvm_getSRMstats()
{
    subject="Querying SRM space"
    debug_test "$subject"
    cmd="$REM_CMD df -k -P /SR-$1/SRM"
    `${cmd} > ${LOGFILE}`
    ret=$?
    if [ $ret -gt 0 ]; then
        debug_result 1
        return 1
    else
        debug_result 0
    fi
    SRM_TOTAL=`cat ${LOGFILE} | awk 'NR == 2{print $2}'`
    SRM_USED=`cat ${LOGFILE} | awk 'NR == 2{print $3}'`
    SRM_UNUSED=`cat ${LOGFILE} | awk 'NR == 2{print $4}'`
    return 0
}

############################################################
## Host Operational Tasks
############################################################

lvm_smDelete()
{
    subject="Delete SR"
    debug_test "$subject"
    cmd="$REM_CMD lvremove -f /dev/VG_XenStorage-$1/SRM >& /dev/null"
    `$cmd`

    cmd="$REM_CMD vgremove VG_XenStorage-$1 >& /dev/null"
    `$cmd`

    cmd="$REM_CMD pvremove $2 >& /dev/null"
    `$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
        debug_result 1
        return 1
    else
        debug_result 0
    fi
    return 0
}

lvm_verify_createSR()
{
    subject="Verify SR"
    debug_test "$subject"
    cmd="$REM_CMD vgs --noheadings VG_XenStorage-$1 >& /dev/null"
    `$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
        debug_result 1
        return 1
    else
        debug_result 0
    fi
    return 0
}

lvm_verify_statSR()
{
    subject="Verifying SR space"
    debug_test "$subject"
    cmd="$REM_CMD vgs --noheadings --units b VG_XenStorage-$1"
    output=`$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
        debug_result 1
        return 1
    fi
    phys=`echo $output | awk '{gsub(/B/,"");print $6}'`
    free=`echo $output | awk '{gsub(/B/,"");print $7}'`
    alloc=`expr $phys - $free`
    if [ $phys != $SR_PHYS -o $alloc != $SR_ALLOC ]; then
        debug_result 1
        incr_exitval
    else
        debug_result 0
    fi
    return
}

lvm_verify_switchSR()
{
    subject="Verifying SR switch"
    debug_test "$subject"
    cmd="$REM_CMD cat /etc/smtab | cut -f1"
    output=`$cmd`
    ret=$?
    if [ $ret -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi

    if [ $1 != $output ]; then
        debug_result 1
        incr_exitval
    else
        debug_result 0
    fi
    return
}

############################################################
## VM Operational Task Verification
############################################################

# Arg 1 -> SRID
# Arg 2 -> VMID
lvm_verify_installVM()
{
    verify_vmconfig $1 $2
    lvm_vdi_count $1 $2
}

#Args: SRID, VMID, size (MBs), disk-name
lvm_verify_addVDI()
{
    lvm_vdi_count $1 $2
    subject="Verify number of disks"
    debug_test "$subject"
    if [ $disks != $GLOBAL_RET ]; then
        debug_result 1
        incr_exitval
    else
        debug_result 0 
    fi

    #Verify disk stats post disk add
    lvm_verify_statSR $1
    subject="Checking SR stats"
    debug_test "$subject"
    size=`expr $3 \* 1024 \* 1024`
    if [ $cur_phys != $SR_PHYS -o $SR_ALLOC != `expr $cur_alloc + $size` ]; then
        debug_result 1
        incr_exitval
    else
        debug_result 0 
    fi

    return ${EXITVAL}
}

lvm_verify_cloneVM()
{
    lvm_verify_installVM $1 $2
    return
}

lvm_verify_addmaxVDI()
{
    return
}

lvm_verify_extendmaxVDI()
{
    return
}

lvm_verify_deleteVDI()
{
    subject="Verify deleteVDI"
    debug_test "$subject"
    cmd="$REM_CMD lvs | grep $2.$3 > /dev/null"
    `$cmd`
    if [ $? == 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi
}

lvm_verify_deleteCloningVM()
{
    return
}

lvm_verify_bootVM()
{
    return
}

lvm_verify_suspendVM()
{
    return
}

lvm_verify_restoreVM()
{
    return
}



############################################################
## SR Driver Operational Task Verification
############################################################
#Args: newSRID
lvm_verify_smCreate()
{
    lvm_verify_createSR $1
    return $?
}

lvm_verify_smAttach()
{
    subject="Verify smAttach"
    debug_test "$subject"
    cmd="$REM_CMD test -d /SR-$1 && test -f /SR-$1/SRM/SRM.dat && xenstore-read /SR/$1/SRM/phys_size"
    output=`$cmd`
    if [ $? -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi 
}

lvm_verify_smDetach()
{
    subject="Verify smDetach"
    debug_test "$subject"
    cmd="$REM_CMD test -d /SR-$1"
    `$cmd`
    if [ $? == 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi

    cmd="$REM_CMD xenstore-read /SR/$1/SRM/phys_size >& /dev/null"
    `$cmd`
    if [ $? == 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi
}

lvm_verify_smAddVdi()
{
    subject="Verify smAddVdi"
    debug_test "$subject"
    cmd="$REM_CMD test -b /dev/VG_XenStorage-$1/$2"
    if [ $? -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi
}

# TODO unused
lvm_verify_smAttachVdi()
{
    subject="Verify smAttachVdi"
    debug_test "$subject"
    cmd="$REM_CMD test -L /SR-$1/images/$2"
    `$cmd`
    if [ $? -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi 
}

lvm_verify_smDetachVdi()
{
    subject="Verify smDetachVdi"
    debug_test "$subject"
    cmd="$REM_CMD test -L /SR-$1/images/$2"
    `$cmd`
    if [ $? == 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi 
}

lvm_verify_smExtendVdi()
{
    return
}

lvm_verify_smCopyVdi()
{
    lvm_vdi_count $3 $2
    subject="Verify smCopyVdi"
    debug_test "$subject"
    if [ ${GLOBAL_RET} != 1 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi
}

lvm_verify_smDeleteVdi()
{
    subject="Verify smdeleteVDI"
    debug_test "$subject"
    cmd="$REM_CMD lvs | grep $2 > /dev/null"
    `$cmd`
    if [ $? == 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi
}

#TODO: Make implementation
lvm_verify_smDelete()
{
    subject="Verify smdelete"
    debug_test "$subject"
    debug_result 0
    return 0
}
