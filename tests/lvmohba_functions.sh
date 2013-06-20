#!/bin/bash


############################################################
## Generic helper functions
############################################################

lvmohba_vdi_count()
{
    return
}


############################################################
## Host Operational Tasks
############################################################

lvmohba_verify_createSR()
{
    return
}

lvmohba_verify_statSR()
{
    return
}

lvmohba_verify_switchSR()
{
    return ${EXITVAL}
}

############################################################
## VM Operational Task Verification
############################################################

# Arg 1 -> SRID
# Arg 2 -> VMID
lvmohba_verify_installVM()
{
    return
}

#Args: SRID, VMID, size (MBs), disk-name
lvmohba_verify_addVDI()
{
    return
}

lvmohba_verify_cloneVM()
{
    return
}

lvmohba_verify_addmaxVDI()
{
    return
}

lvmohba_verify_extendmaxVDI()
{
    return
}

lvmohba_verify_deleteVDI()
{
    return
}

lvmohba_verify_deleteCloningVM()
{
    return
}

lvmohba_verify_bootVM()
{
    return
}

lvmohba_verify_suspendVM()
{
    return
}

lvmohba_verify_restoreVM()
{
    return
}



############################################################
## SR Driver Operational Task Verification
############################################################

lvmohba_verify_smCreate()
{
    subject="Verify smCreate"
    debug_test "$subject"

    debug_result 0

    return
}

lvmohba_verify_smAttach()
{
    return
}

lvmohba_verify_smDetach()
{
    return
}

lvmohba_verify_smDelete()
{
    subject="Verify smDelete"
    debug_test "$subject"
    debug_result 0
    return
}

lvmohba_verify_smAddVdi()
{
    subject="Verify smAddVdi"
    debug_test "$subject"
    debug_result 0

    return
}

lvmohba_verify_smAttachVdi()
{
    return
}

lvmohba_verify_smDetachVdi()
{
    return
}

lvmohba_verify_smExtendVdi()
{
    return
}

lvmohba_verify_smCopyVdi()
{
    return
}

lvmohba_verify_smDeleteVdi()
{
    return
}
