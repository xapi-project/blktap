#!/bin/bash


############################################################
## Generic helper functions
############################################################

lvmoiscsi_vdi_count()
{
    return
}


############################################################
## Host Operational Tasks
############################################################

lvmoiscsi_verify_createSR()
{
    return
}

lvmoiscsi_verify_statSR()
{
    return
}

lvmoiscsi_verify_switchSR()
{
    return ${EXITVAL}
}

############################################################
## VM Operational Task Verification
############################################################

# Arg 1 -> SRID
# Arg 2 -> VMID
lvmoiscsi_verify_installVM()
{
    return
}

#Args: SRID, VMID, size (MBs), disk-name
lvmoiscsi_verify_addVDI()
{
    return
}

lvmoiscsi_verify_cloneVM()
{
    return
}

lvmoiscsi_verify_addmaxVDI()
{
    return
}

lvmoiscsi_verify_extendmaxVDI()
{
    return
}

lvmoiscsi_verify_deleteVDI()
{
    return
}

lvmoiscsi_verify_deleteCloningVM()
{
    return
}

lvmoiscsi_verify_bootVM()
{
    return
}

lvmoiscsi_verify_suspendVM()
{
    return
}

lvmoiscsi_verify_restoreVM()
{
    return
}



############################################################
## SR Driver Operational Task Verification
############################################################

lvmoiscsi_verify_smCreate()
{
    subject="Verify smCreate"
    debug_test "$subject"

    debug_result 0

    return
}

lvmoiscsi_verify_smAttach()
{
    return
}

lvmoiscsi_verify_smDetach()
{
    return
}

lvmoiscsi_verify_smDelete()
{
    subject="Verify smDelete"
    debug_test "$subject"
    debug_result 0
    return
}

lvmoiscsi_verify_smAddVdi()
{
    subject="Verify smAddVdi"
    debug_test "$subject"
    debug_result 0

    return
}

lvmoiscsi_verify_smAttachVdi()
{
    return
}

lvmoiscsi_verify_smDetachVdi()
{
    return
}

lvmoiscsi_verify_smExtendVdi()
{
    return
}

lvmoiscsi_verify_smCopyVdi()
{
    return
}

lvmoiscsi_verify_smDeleteVdi()
{
    return
}
