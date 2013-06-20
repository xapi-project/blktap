#!/bin/bash


############################################################
## Generic helper functions
############################################################

netapp_vdi_count()
{
    return
}


############################################################
## Host Operational Tasks
############################################################

netapp_verify_createSR()
{
    return
}

netapp_verify_statSR()
{
    return
}

netapp_verify_switchSR()
{
    return ${EXITVAL}
}

############################################################
## VM Operational Task Verification
############################################################

# Arg 1 -> SRID
# Arg 2 -> VMID
netapp_verify_installVM()
{
    return
}

#Args: SRID, VMID, size (MBs), disk-name
netapp_verify_addVDI()
{
    return
}

netapp_verify_cloneVM()
{
    return
}

netapp_verify_addmaxVDI()
{
    return
}

netapp_verify_extendmaxVDI()
{
    return
}

netapp_verify_deleteVDI()
{
    return
}

netapp_verify_deleteCloningVM()
{
    return
}

netapp_verify_bootVM()
{
    return
}

netapp_verify_suspendVM()
{
    return
}

netapp_verify_restoreVM()
{
    return
}



############################################################
## SR Driver Operational Task Verification
############################################################

netapp_verify_smCreate()
{
    subject="Verify smCreate"
    debug_test "$subject"

    debug_result 0

    return
}

netapp_verify_smAttach()
{
    return
}

netapp_verify_smDetach()
{
    return
}

netapp_verify_smDelete()
{
    subject="Verify smDelete"
    debug_test "$subject"
    debug_result 0
    return
}

netapp_verify_smAddVdi()
{
    subject="Verify smAddVdi"
    debug_test "$subject"
    debug_result 0

    return
}

netapp_verify_smAttachVdi()
{
    return
}

netapp_verify_smDetachVdi()
{
    return
}

netapp_verify_smExtendVdi()
{
    return
}

netapp_verify_smCopyVdi()
{
    return
}

netapp_verify_smDeleteVdi()
{
    return
}
