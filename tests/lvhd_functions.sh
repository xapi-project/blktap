#!/bin/bash


############################################################
## Generic helper functions
############################################################

lvhd_vdi_count()
{
    return
}

#Downsize disk - Used for debugging speedup
# Arg 1 -> VMID
# Arg 2 -> Size
# Arg 3 -> Name
# Arg 4 -> SRID
lvhd_downsize_disk()
{
    if [ ${CLONE_DEBUG} == 0 ]; then
	return 0
    fi

    return
}


############################################################
## Host Operational Tasks
############################################################

lvhd_verify_createSR()
{
    return
}

lvhd_verify_statSR()
{
    return
}

lvhd_verify_switchSR()
{
    return ${EXITVAL}
}

############################################################
## VM Operational Task Verification
############################################################

# Arg 1 -> SRID
# Arg 2 -> VMID
lvhd_verify_installVM()
{
    return
}

#Args: SRID, VMID, size (MBs), disk-name
lvhd_verify_addVDI()
{
    return
}

lvhd_verify_cloneVM()
{
    return
}

lvhd_verify_addmaxVDI()
{
    return
}

lvhd_verify_extendmaxVDI()
{
    return
}

lvhd_verify_deleteVDI()
{
    return
}

lvhd_verify_deleteCloningVM()
{
    return
}

lvhd_verify_bootVM()
{
    return
}

lvhd_verify_suspendVM()
{
    return
}

lvhd_verify_restoreVM()
{
    return
}



############################################################
## SR Driver Operational Task Verification
############################################################

lvhd_verify_smCreate()
{
    subject="Verify smCreate"
    debug_test "$subject"

    debug_result 0

    return
}

lvhd_verify_smAttach()
{
    return
}

lvhd_verify_smDetach()
{
    return
}

lvhd_verify_smDelete()
{
    subject="Verify smDelete"
    debug_test "$subject"
    debug_result 0
    return
}

lvhd_verify_smAddVdi()
{
    subject="Verify smAddVdi"
    debug_test "$subject"
    debug_result 0

    return
}

lvhd_verify_smAttachVdi()
{
    return
}

lvhd_verify_smDetachVdi()
{
    return
}

lvhd_verify_smExtendVdi()
{
    return
}

lvhd_verify_smCopyVdi()
{
    return
}

lvhd_verify_smDeleteVdi()
{
    subject="Verify smDeleteVdi $1 - $2 - $3"
    debug_test "$subject"
    cmd="$REM_CMD test -e $2/$1/$3"
    output=`$cmd`
    if [ $? == 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi       
    return
}
