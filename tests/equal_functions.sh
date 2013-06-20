#!/bin/bash


############################################################
## Generic helper functions
############################################################

equal_vdi_count()
{
    return
}


############################################################
## Host Operational Tasks
############################################################

equal_verify_createSR()
{
    return
}

equal_verify_statSR()
{
    return
}

equal_verify_switchSR()
{
    return ${EXITVAL}
}

############################################################
## VM Operational Task Verification
############################################################

# Arg 1 -> SRID
# Arg 2 -> VMID
equal_verify_installVM()
{
    return
}

#Args: SRID, VMID, size (MBs), disk-name
equal_verify_addVDI()
{
    return
}

equal_verify_cloneVM()
{
    return
}

equal_verify_addmaxVDI()
{
    return
}

equal_verify_extendmaxVDI()
{
    return
}

equal_verify_deleteVDI()
{
    return
}

equal_verify_deleteCloningVM()
{
    return
}

equal_verify_bootVM()
{
    return
}

equal_verify_suspendVM()
{
    return
}

equal_verify_restoreVM()
{
    return
}



############################################################
## SR Driver Operational Task Verification
############################################################

equal_verify_smCreate()
{
    subject="Verify smCreate"
    debug_test "$subject"

    debug_result 0

    return
}

equal_verify_smAttach()
{
    return
}

equal_verify_smDetach()
{
    return
}

equal_verify_smDelete()
{
    subject="Verify smDelete"
    debug_test "$subject"
    debug_result 0
    return
}

equal_verify_smAddVdi()
{
    subject="Verify smAddVdi"
    debug_test "$subject"
    debug_result 0

    return
}

equal_verify_smAttachVdi()
{
    return
}

equal_verify_smDetachVdi()
{
    return
}

equal_verify_smExtendVdi()
{
    return
}

equal_verify_smCopyVdi()
{
    return
}

equal_verify_smDeleteVdi()
{
    return
}
