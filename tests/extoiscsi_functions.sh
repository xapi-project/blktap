#!/bin/bash


############################################################
## Generic helper functions
############################################################

extoiscsi_vdi_count()
{
    return
}


############################################################
## Host Operational Tasks
############################################################

extoiscsi_verify_createSR()
{
    return
}

extoiscsi_verify_statSR()
{
    return
}

extoiscsi_verify_switchSR()
{
    return ${EXITVAL}
}

############################################################
## VM Operational Task Verification
############################################################

# Arg 1 -> SRID
# Arg 2 -> VMID
extoiscsi_verify_installVM()
{
    return
}

#Args: SRID, VMID, size (MBs), disk-name
extoiscsi_verify_addVDI()
{
    return
}

extoiscsi_verify_cloneVM()
{
    return
}

extoiscsi_verify_addmaxVDI()
{
    return
}

extoiscsi_verify_extendmaxVDI()
{
    return
}

extoiscsi_verify_deleteVDI()
{
    return
}

extoiscsi_verify_deleteCloningVM()
{
    return
}

extoiscsi_verify_bootVM()
{
    return
}

extoiscsi_verify_suspendVM()
{
    return
}

extoiscsi_verify_restoreVM()
{
    return
}



############################################################
## SR Driver Operational Task Verification
############################################################

extoiscsi_verify_smCreate()
{
    subject="Verify smCreate"
    debug_test "$subject"

    debug_result 0

    return
}

extoiscsi_verify_smAttach()
{
    return
}

extoiscsi_verify_smDetach()
{
    return
}

extoiscsi_verify_smDelete()
{
    subject="Verify smDelete"
    debug_test "$subject"
    debug_result 0
    return
}

extoiscsi_verify_smAddVdi()
{
    subject="Verify smAddVdi"
    debug_test "$subject"
    debug_result 0

    return
}

extoiscsi_verify_smAttachVdi()
{
    return
}

extoiscsi_verify_smDetachVdi()
{
    return
}

extoiscsi_verify_smExtendVdi()
{
    return
}

extoiscsi_verify_smCopyVdi()
{
    return
}

extoiscsi_verify_smDeleteVdi()
{
    return
}
