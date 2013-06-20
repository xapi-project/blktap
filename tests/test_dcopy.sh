#!/bin/bash
## Set of SM tests for default SR

## Source the general system function file
. ./XE_api_library.sh

## source performance tests
. ./performance_functions.sh

# Ensures that the tp and dcopy binaries installed.
check_dcopy_binaries()
{
    cmd="$REM_CMD test -f /opt/xensource/debug/tp"
    run "$cmd"
    if [ $RUN_RC -gt 0 ]; then 
	debug "The test pattern binary is missing."
	debug "Please install tp under /opt/xensource/debug and re-run."
	debug "Exiting quietly."
	exit
    fi

    cmd="$REM_CMD test -f /opt/xensource/libexec/dcopy"
    run "$cmd"
    if [ $RUN_RC -gt 0 ]; then 
	debug "The dcopy binary is missing."
	debug "Please install dcopy under /opt/xensource/libexec and re-run."
	debug "Exiting quietly."
	exit
    fi
}

# Creates a $SUBSTRATE_TYPE SR.
setup_sr() 
{
    smCreate "${SUBSTRATE_TYPE}" "${CONTENT_TYPE}" "${DEVSTRING}" \
        "${NFSSERVER}" "${NFSSERVERPATH}" "${IQN_INITIATOR_ID}" \
        "${LISCSI_TARGET_IP}" "${LISCSI_TARGET_ID}" "${LISCSI_ISCSI_ID}"
    test_exit 1
}

# Destroys the specified SR.
# Arguments:
# 1: the UUID of the SR to destroy
cleanup_sr()
{
    smDelete ${1}
    test_exit 0
}

# FIXME redundant
replug_vbd() {
    local VBD_ID=$1

    smUnplugVbd ${VBD_ID}
    test_exit 1
    smPlugVbd ${VBD_ID}
    test_exit 1

    sleep 1
}

# Unplugs and deletes the VBD, then deletes the VDI.
cleanup_vbd()
{
    smUnplugVbd $1
    test_exit 1

    smDeleteVbd $1
    test_exit 1

    smDeleteVdi $3 ${DEVSTRING} $2
    test_exit 1
}

# FIXME redundant
# Args:
# 1 -> DEVICE
# 2 -> chunksizeKB
# 3 -> iterations
# 4 -> pad bytes
generate_pattern()
{
    debug "Generating pattern on SRC device ${1} with parameters:"
    debug "      Chunksize : $2KB"
    debug "      Iterations: $3" 
    debug "      Pad Bytes : $4B"

    subject="Writing disk test pattern"
    debug_test "$subject"

    cmd="$REM_CMD /opt/xensource/debug/tp $1 $2 $3 $4"
    run "$cmd"
    if [ $RUN_RC -gt 0 ]; then
	debug_result 1
        incr_exitval
	return 1
    fi
    debug_result 0
}

# TODO What does dcopy do?
# Args:
# 1 -> Options
# 2 -> SRC DEVICE
# 3 -> DST DEVICE
copy_pattern()
{
    subject="Copying disk contents"
    debug_test "$subject"

    cmd="$REM_CMD /opt/xensource/libexec/dcopy $1 $2 $3"
    run "$cmd"
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi
    debug_result 0
    debug ""
}

# FIXME redundant
verify_disks()
{
    subject="Verifying disk contents"
    debug_test "$subject"

    cmd="$REM_CMD diff $1 $2 >> /dev/null"
    run "$cmd"
    if [ $RUN_RC -gt 0 ]; then
	debug_result 1
        incr_exitval
	return 1
    fi
    debug_result 0
}

# Performs a dcopy test: adds a VDI on each SR (there must be two of them),
# generates a pattern on the first VDI, replugs it, execute the dcopy from the
# first VDI to the second one, replug the second one, and verify that the
# contents of the two VDIs are the same.
# Args:
# 1 -> chunksizeKB
# 2 -> iterations
# 3 -> pad bytes
# 4 -> dcopy options
run_dcopy_test()
{
    # Check SRs and lookup DRIVER_TYPE
    smCheckSR ${SRC_SR}
    test_exit 1
    DRIVER_TYPE1=${SR_DRIVER}

    smCheckSR ${DST_SR}
    test_exit 1
    DRIVER_TYPE2=${SR_DRIVER}

    debug ""
    debug "Running tests on Source SR type:                   ${DRIVER_TYPE1}"
    debug "            Destination SR type:                   ${DRIVER_TYPE2}"
    debug "                       VDI Size:                   ${VDI_SIZE}"
    debug ""

    # Create VDIs
    DRIVER_TYPE=${DRIVER_TYPE1}
    smAddVdi ${SRC_SR} ${DEVSTRING} "TestVDI" ${VDI_SIZE}
    test_exit 1
    SRC_VDI=${VDI_ID}

    DRIVER_TYPE=${DRIVER_TYPE2}
    smAddVdi ${DST_SR} ${DEVSTRING} "TestVDI" ${VDI_SIZE}
    test_exit 1
    DST_VDI=${VDI_ID}

    # Plug src disk in
    smCreateVbd ${SRC_VDI} ${DOM0_ID} 'autodetect' 
    test_exit 1
    SRC_VBD_ID=${VBD_ID}

    smPlugVbd ${SRC_VBD_ID}
    test_exit 1

    smGetVbdDevice ${SRC_VBD_ID}
    test_exit 1

    SRCDEVPATH="/dev/${VBD_DEVICE}"
    debug "Plugged SRC device ${SRCDEVPATH}"

    #Insert a test pattern
    generate_pattern ${SRCDEVPATH} $1 $2 $3
    test_exit 1

    # Replug the SRC VDI
    replug_vbd ${SRC_VBD_ID}
    test_exit 1

    smGetVbdDevice ${SRC_VBD_ID}
    test_exit 1

    SRCDEVPATH="/dev/${VBD_DEVICE}"
    debug "Re-Plugged SRC device ${SRCDEVPATH}"

    # Pug the DST VDI
    smCreateVbd ${DST_VDI} ${DOM0_ID} 'autodetect' 
    test_exit 1
    DST_VBD_ID=${VBD_ID}

    smPlugVbd ${DST_VBD_ID}
    test_exit 1

    smGetVbdDevice ${DST_VBD_ID}
    test_exit 1

    DSTDEVPATH="/dev/${VBD_DEVICE}"
    debug "Plugged DST device ${DSTDEVPATH}"

    OPT=$4
    if [ ! -z $OPT ]; then
        if [[ ! ${DRIVER_TYPE2} == 'nfs' && ! ${DRIVER_TYPE2} == 'file' ]]; then
            debug "Resetting Sparseness operator for SR type that doesn't support it [${DRIVER_TYPE2}]"
            OPT=""
        fi
    fi
    copy_pattern ${OPT} ${SRCDEVPATH} ${DSTDEVPATH}
    test_exit 1

    # Replug the DST VDI
    replug_vbd ${DST_VBD_ID}
    test_exit 1

    smGetVbdDevice ${DST_VBD_ID}
    test_exit 1

    DSTDEVPATH="/dev/${VBD_DEVICE}"
    debug "Re-Plugged DST device ${DSTDEVPATH}"

    verify_disks ${SRCDEVPATH} ${DSTDEVPATH}
    test_exit 1

    DRIVER_TYPE=${DRIVER_TYPE1}
    cleanup_vbd ${SRC_VBD_ID} ${SRC_VDI} ${SRC_SR}

    DRIVER_TYPE=${DRIVER_TYPE2}
    cleanup_vbd ${DST_VBD_ID} ${DST_VDI} ${DST_SR}
}

# Performs run_dcopy_test's using various options.
run_tests()
{
    # Defaults:
    CHUNKSZ=1
    PAD=0
    OPT=""
    
    # Test 1a - Chunksize 1KB, zero pad bytes, no sparseness
    ITER=$((($VDI_SIZE - $PAD)/ ($CHUNKSZ*2*1024)))
    run_dcopy_test ${CHUNKSZ} ${ITER} ${PAD} ${OPT}
    test_exit 1

    # Test 1b - Chunksize 1KB, zero pad bytes, with sparseness
    ITER=$((($VDI_SIZE - $PAD)/ ($CHUNKSZ*2*1024)))
    OPT="--sparse"
    run_dcopy_test ${CHUNKSZ} ${ITER} ${PAD} ${OPT}
    test_exit 1

    # Test 2a - Chunksize 10KB, zero pad bytes, no sparseness
    CHUNKSZ=10
    ITER=$((($VDI_SIZE - $PAD)/ ($CHUNKSZ*2*1024)))
    OPT=""
    run_dcopy_test ${CHUNKSZ} ${ITER} ${PAD} ${OPT}
    test_exit 1

    # Test 2b - Chunksize 10KB, zero pad bytes, with sparseness
    ITER=$((($VDI_SIZE - $PAD)/ ($CHUNKSZ*2*1024)))
    OPT="--sparse"
    run_dcopy_test ${CHUNKSZ} ${ITER} ${PAD} ${OPT}
    test_exit 1

    # Test 3a - Chunksize 1KB, 5 pad bytes, no sparseness
    CHUNKSZ=1
    PAD=5
    ITER=$((($VDI_SIZE - $PAD)/ ($CHUNKSZ*2*1024)))
    OPT=""
    run_dcopy_test ${CHUNKSZ} ${ITER} ${PAD} ${OPT}
    test_exit 1

    # Test 3b - Chunksize 1KB, 5 pad bytes, with sparseness
    ITER=$((($VDI_SIZE - $PAD)/ ($CHUNKSZ*2*1024)))
    OPT="--sparse"
    run_dcopy_test ${CHUNKSZ} ${ITER} ${PAD} ${OPT}
    test_exit 1
}

# Performs the dcopy tests from one SR to another and vice versa.
# Args:
# 1 -> First SR
# 2 -> Second SR
run_sr_tests()
{
    #Run tests over both SRs, flipping SRC and DST
    SRC_SR=${1}
    DST_SR=${2}
    run_tests

    SRC_SR=${2}
    DST_SR=${1}
    run_tests
}

# XXX Similar to test1.sh

TEMPLATE_ALIAS=windows

process_arguments $@

post_process

check_req_args

check_req_sw

install_ssh_certificate ${REMHOSTNAME} ${SSH_PRIVATE_KEY} ${PASSWD}

check_dcopy_binaries

print_version_info

gen_hostlist

if [[ -z ${NFSSERVER}  || -z ${NFSSERVERPATH} ]] ; then
    debug "No NFS information specified. Exiting quietly"
    exit
fi

if [[ -z ${IQN_INITIATOR_ID} || -z ${LISCSI_TARGET_IP} || -z ${LISCSI_TARGET_ID} || -z ${LISCSI_ISCSI_ID} ]]; then
    debug "iSCSI configuration information missing. Exiting quietly"
    exit
fi

#Setup Local SR, NFS SR and LVMoISCSI SR
# Type 1 - Local EXT
DRIVER_TYPE=file
SUBSTRATE_TYPE=ext
CONTENT_TYPE=local_file
setup_sr
SR_ID1=${SR_ID}

# Type 2 - NFS
DRIVER_TYPE=nfs
SUBSTRATE_TYPE=nfs
CONTENT_TYPE=nfs
setup_sr
SR_ID2=${SR_ID}

# Type 3 - LVMoISCSI
DRIVER_TYPE=lvmoiscsi    
SUBSTRATE_TYPE=lvmoiscsi
CONTENT_TYPE=user
setup_sr
SR_ID3=${SR_ID}

if [ -n $FAST ]; then
    # Fast test - 4MB disks
    VDI_SIZE=$((4*1024*1024))
else
    # Full test - 4GB disks
    VDI_SIZE=$((4*1024*1024*1024))
fi

# SR Test matrix combinations:
# Test 1:       SR_ID1 <-> SR_ID2
# Test 2:       SR_ID1 <-> SR_ID3
# Test 3:       SR_ID2 <-> SR_ID3

run_sr_tests ${SR_ID1} ${SR_ID2}
run_sr_tests ${SR_ID1} ${SR_ID3}
run_sr_tests ${SR_ID2} ${SR_ID3}

# Cleanup SRs
cleanup_sr ${SR_ID1}
cleanup_sr ${SR_ID2}
cleanup_sr ${SR_ID3}

print_exit_info 
