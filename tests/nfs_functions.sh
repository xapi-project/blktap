#!/bin/bash


############################################################
## Generic helper functions
############################################################

nfs_vdi_count()
{
    return
}


############################################################
## Host Operational Tasks
############################################################

nfs_verify_createSR()
{
    return
}

nfs_verify_statSR()
{
    return
}

nfs_verify_switchSR()
{
    return ${EXITVAL}
}

############################################################
## VM Operational Task Verification
############################################################

# Arg 1 -> SRID
# Arg 2 -> VMID
nfs_verify_installVM()
{
    return
}

#Args: SRID, VMID, size (MBs), disk-name
nfs_verify_addVDI()
{
    return
}

nfs_verify_cloneVM()
{
    return
}

nfs_verify_addmaxVDI()
{
    return
}

nfs_verify_extendmaxVDI()
{
    return
}

nfs_verify_deleteVDI()
{
    return
}

nfs_verify_deleteCloningVM()
{
    return
}

nfs_verify_bootVM()
{
    return
}

nfs_verify_suspendVM()
{
    return
}

nfs_verify_restoreVM()
{
    return
}



############################################################
## SR Driver Operational Task Verification
############################################################

nfs_verify_smCreate()
{
    subject="Verify smCreate"
    debug_test "$subject"

    debug_result 0

    return
}

nfs_verify_smAttach()
{
    return
}

nfs_verify_smDetach()
{
    return
}

nfs_verify_smDelete()
{
    subject="Verify smDelete"
    debug_test "$subject"
    debug_result 0
    return
}

nfs_verify_smAddVdi()
{
    subject="Verify smAddVdi"
    debug_test "$subject"
    debug_result 0

    return
}

nfs_verify_smAttachVdi()
{
    return
}

nfs_verify_smDetachVdi()
{
    return
}

nfs_verify_smExtendVdi()
{
    return
}

nfs_verify_smCopyVdi()
{
    return
}

nfs_verify_smDeleteVdi()
{
    return
}
