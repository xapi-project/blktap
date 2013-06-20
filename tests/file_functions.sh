#!/bin/bash


############################################################
## Generic helper functions
############################################################

file_vdi_count()
{
    return
}

#Downsize disk - Used for debugging speedup
# Arg 1 -> VMID
# Arg 2 -> Size
# Arg 3 -> Name
# Arg 4 -> SRID
file_downsize_disk()
{
    if [ ${CLONE_DEBUG} == 0 ]; then
	return 0
    fi

    return
}


############################################################
## Host Operational Tasks
############################################################

file_verify_createSR()
{
    return
}

file_verify_statSR()
{
    return
}

file_verify_switchSR()
{
    return ${EXITVAL}
}

############################################################
## VM Operational Task Verification
############################################################

# Arg 1 -> SRID
# Arg 2 -> VMID
file_verify_installVM()
{
    return
}

#Args: SRID, VMID, size (MBs), disk-name
file_verify_addVDI()
{
    return
}

file_verify_cloneVM()
{
    return
}

file_verify_addmaxVDI()
{
    return
}

file_verify_extendmaxVDI()
{
    return
}

file_verify_deleteVDI()
{
    return
}

file_verify_deleteCloningVM()
{
    return
}

file_verify_bootVM()
{
    return
}

file_verify_suspendVM()
{
    return
}

file_verify_restoreVM()
{
    return
}



############################################################
## SR Driver Operational Task Verification
############################################################

file_verify_smCreate()
{
    subject="Verify smCreate"
    debug_test "$subject"

    debug_result 0

    return
}

file_verify_smAttach()
{
    return
}

file_verify_smDetach()
{
    return
}

file_verify_smDelete()
{
    subject="Verify smDelete"
    debug_test "$subject"
    debug_result 0
    return
}

file_verify_smAddVdi()
{
    subject="Verify smAddVdi"
    debug_test "$subject"
    debug_result 0

    return
}

file_verify_smAttachVdi()
{
    return
}

file_verify_smDetachVdi()
{
    return
}

file_verify_smExtendVdi()
{
    return
}

file_verify_smCopyVdi()
{
    return
}

file_verify_smDeleteVdi()
{
    subject="Verify smDeleteVdi"
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
