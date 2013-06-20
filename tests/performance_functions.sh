#!/bin/bash

################################
## VM MANAGEMENT
################################

# An alternative wrapper using a password, CARBON_DOMU_PASSWORD, to login. To
# be used if a trusted key isn't available.
ssh_password_wrapper() {
    local COMMAND=$1
    shift

    local EXPECT_SCRIPT=`mktemp /tmp/sshexpectXXXX`
    
#    echo "#!/usr/bin/expect --" >> ${EXPECT_SCRIPT}
#    echo "" >> ${EXPECT_SCRIPT}
#    echo "spawn ${COMMAND} -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null -oPubkeyAuthentication=no $@" >> ${EXPECT_SCRIPT}
#    echo "set timeout 15" >> ${EXPECT_SCRIPT}
#    echo "expect {" >> ${EXPECT_SCRIPT}
#    echo -e "\t\"*assword:*\" {" >> ${EXPECT_SCRIPT}
#    echo -e "\t\tsend \"${CARBON_DOMU_PASSWORD}\\r\"" >> ${EXPECT_SCRIPT}
#    echo -e "\t\tinteract" >> ${EXPECT_SCRIPT}
#    echo -e "\t}" >> ${EXPECT_SCRIPT}
#    echo -e "\ttimeout {" >> ${EXPECT_SCRIPT}
#    echo -e "\t\texit 1" >> ${EXPECT_SCRIPT}
#    echo -e "\t}" >> ${EXPECT_SCRIPT}
#    echo -e "}" >> ${EXPECT_SCRIPT}

    echo "#!/usr/bin/expect --" >> ${EXPECT_SCRIPT}
    echo "" >> ${EXPECT_SCRIPT}
    echo "spawn ${COMMAND} -oStrictHostKeyChecking=no -oUserKnownHostsFile=/dev/null -oPubkeyAuthentication=no $@" >> ${EXPECT_SCRIPT}
    echo "expect \"*assword:*\"" >> ${EXPECT_SCRIPT}
    echo "send \"${CARBON_DOMU_PASSWORD}\\r\"" >> ${EXPECT_SCRIPT}
    #echo "timeout abort" >> ${EXPECT_SCRIPT}
    echo "expect eof" >> ${EXPECT_SCRIPT}
    #echo "interact" >> ${EXPECT_SCRIPT}
    echo "exit [lindex [wait] 3]" >> ${EXPECT_SCRIPT}
#
    chmod +x ${EXPECT_SCRIPT}
    if ! expect ${EXPECT_SCRIPT}; then
        rm ${EXPECT_SCRIPT}
        return 1
    fi

    # Clean up.
    rm ${EXPECT_SCRIPT}
    return 0
} 

# Installs a virtual machine.
# TODO document arguments
# Arg 1: VM Name
# Arg 2: Memory
# Arg 3: SRID
# Arg 3: MAC
# TODO move elsewhere (nothing to do with perf)
pinstall_VM()
{
    MNTPNT="/tmp/installmnt"
    IMAGEFILE="${MNTPNT}/Debian_Etch.xva"
    debug_test "Installing VM"
    cmd="$REM_CMD $CMD network-list"
    run $cmd
    echo $RUN_OUTPUT | grep -q -E "$BRIDGE" &> /dev/null
    if [ $? -ne 0 ] ; then
        debug_result 1
        debug "ERROR: Invalid bridge $BRIDGE"
        incr_exitval
        return 1
    fi
    
    if [ -n "$VM_INSTALL_CMD" ]; then
        cmd="$VM_INSTALL_CMD $1 $2 $3 $3"
    elif [ -n "$VM_IMAGE_LOCATION" ]; then
        #On an embedded edition without Debian templates
        #the VM_IMAGE_LOCATION specifies a nfs export point
        #from where Debian image can be imported
        run "$REM_CMD mkdir -p ${MNTPNT}"
        if [ $? -ne 0 ] ; then debug "mkdir failed" ; debug_result 1; incr_exitval ; return 1; fi
        run "$REM_CMD mount -t nfs $VM_IMAGE_LOCATION ${MNTPNT}"
        if [ $? -ne 0 ] ; then debug "mount failed" ; debug_result 1; incr_exitval ; return 1; fi
        # Import the VM image
        if [ -n "$3" ]; then
            cmd="$REM_CMD $CMD vm-import filename=${IMAGEFILE} sr-uuid=$3"
        else
            cmd="$REM_CMD $CMD vm-import filename=${IMAGEFILE}"
        fi

        run $cmd
        if [ $RUN_RC -gt 0 ]; then
            debug_result 1
            incr_exitval
            exit 1
        else
            debug_result 0

            cmd="$REM_CMD $CMD vm-list name-label=\"$1\" --minimal"
        fi
    else
        cmd="$REM_CMD $CMD vm-install $ARGS new-name-label=\"$1\" template=\"$TEMPLATE\""
    fi
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        #${DRIVER_TYPE}_verify_installVM $3 $vmid

        vmid=`echo "$RUN_OUTPUT" | grep -E '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' -o`
        GLOBAL_RET=$vmid

        # Set VM parameters
        cmd="$REM_CMD $CMD vm-param-set uuid=$vmid name-label=\"$1\" PV-args=noninteractive"
        run $cmd

        if [ -n "$2" ]; then
            cmd="$REM_CMD $CMD vm-param-set uuid=$vmid memory-dynamic-max=${2}MiB memory-dynamic-min=${2}MiB"
            run $cmd

            cmd="$REM_CMD $CMD vm-param-set uuid=$vmid memory-static-max=${2}MiB memory-static-min=${2}MiB"
            run $cmd
        fi

        # get network
        cmd="$REM_CMD $CMD network-list bridge=$BRIDGE --minimal"
        NETWORK_ID=`$cmd`

        #add a vif
        cmd="$REM_CMD $CMD vif-create device=0 network-uuid=$NETWORK_ID vm-uuid=$vmid mac=random"
        run "$cmd"

        if [ -n "$VM_IMAGE_LOCATION" ]; then
            run "$REM_CMD umount ${MNTPNT}"
        fi

        return 0
    fi
}

# Retrieves the IP address of a VM.
# Arg 1: VM_UUID
# TODO possibly redundant?
# TODO move elsewhere (nothing to do with perf)
get_VM_IP()
{
    debug_test "Getting VM IP"
    cmd="$REM_CMD $CMD vm-vif-list $ARGS vm-id=$1"
    line=`$cmd | grep ip`
    if [ $? -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=`echo "$line" | awk '{print $2}'`
        return 0
    fi
}

# Arg 1: VM_NAME
get_VM_domID()
{
    debug_test "Getting VM domain number"
    line=`$REM_CMD $XM list | grep $1`
    if [ $? -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=`echo "$line" | awk '{print $2}'`
        return 0
    fi
}

launch_blktapctrl()
{
    debug_test "Launching blktapctrl"
    started=`$REM_CMD ps -eo comm | grep blktapctrl`
    if [ -z $started ]; then
        output=`$REM_CMD /usr/sbin/blktapctrl`
        if [ $? -gt 0 ]; then
            debug_result 1
            incr_exitval
            return 1
        fi
    fi
    debug_result 0
    return 0
}

# Arg 1: domID
# Arg 2: vbd_name
get_vdev_ID() 
{
    debug_test "Getting VBD's ID"
    devices=`$REM_CMD $XM block-list $1`
    n=`echo "$devices" | awk 'END {print NR}'`
    until [ $n == 0 ]; do
        let n=n-1
        devices=`echo "$devices" | tail -n$n`
        path=`echo "$devices" | head -n1 | awk '{print $7}'`
        name=`$REM_CMD $XSR "$path"/dev`
        if [ $? -gt 0 ]; then
            debug_result 1
            return 1
        elif [ $name == $2 ]; then
            GLOBAL_RET=`echo "$devices" | head -n1 | awk '{print $1}'`
            debug_result 0
            return 0
        fi
    done
    debug_result 1
    return 1
}

# Arg 1: domID
# Arg 2: vdi_path
# Arg 3: vbd_name
block_attach() 
{
    launch_blktapctrl
    debug_test "Attaching block device"
    cmd="$REM_CMD $XM block-attach $1 $2 $3 w"
    output=`$cmd`
    if [ $? -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
    fi
    
    sleep 3
    get_vdev_ID $1 $3
    if [ $? -gt 0 ]; then
        incr_exitval
        return 1
    else
        return 0
    fi
}

# Arg 1: domID
# Arg 2: vdevID
block_detach()
{
    debug_test "Detaching block device"
    cmd="$REM_CMD $XM block-detach $1 $2"
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


# Arg 1: SRID
# Arg 2: vd_uuid
get_VDI_devname()
{
    debug_test "Getting VDI device name"
    devname="/SR-${1}/images/${2}"
    cmd="$REM_CMD stat ${devname} 2>&1 /dev/null"
    output=`$cmd`
    if [ $? -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        GLOBAL_RET=${devname}
        return 0
    fi
}

# After installing a VM perform optional tailoring for running in a
# XenRT environment
# TODO document arguments & functionality
xenrt_tailor()
{
    debug_test "Tailoring VM for XenRT environment"
    if [ -n "${APT_CACHER}" ]; then
        run $DOMU_REM_CMD "echo deb ${APT_CACHER} etch main > /etc/apt/sources.list"
        run $DOMU_REM_CMD "rm -f /etc/apt/sources.list.d/citrix.list"
    fi
}

gcc_install()
{
    debug_test "Installing gcc and dependencies"
    cmd="$DOMU_REM_CMD apt-get update"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi
    cmd="$DOMU_REM_CMD apt-get install -y libc-dev"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi
    cmd="$DOMU_REM_CMD apt-get install -y gcc"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi
}

# Installs bonnie++.
bonnie_install()
{
    debug_test "Installing Bonnie++"
    cmd="$DOMU_REM_CMD apt-get update"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi
    cmd="$DOMU_REM_CMD apt-get install -y fileutils coreutils bonnie++"
    run $cmd
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
        return 0
    fi
}

# Checks a VBD by checking whether the block device exists.  Arguments: 1:
# machine access command (e.g. "ssh some_host", can be empty if it's on the
# same machine) 2: the block device that corresponds to the VBD.  3: number of
# attempts, where between each attempt the function sleeps for one second
# (optional).
check_vbd()
{
    local i
    local ATTEMPTS=$3

    if [ -z "$ATTEMPTS" -o $ATTEMPTS -lt 1 ]; then
        ATTEMPTS=1
    fi

    debug_test "Checking for existence of $2"
    for i in $(seq 1 $ATTEMPTS); do
        run "$1 test -b $2"
        if [ $RUN_RC -eq 0 ]; then
            debug_result 0
            return 0
        fi
        sleep 1s
    done

    debug_result 1
    incr_exitval
    return 1
}

################################
## DATA INTEGRITY TESTS
################################
# Arg1: Machine access Command
# Arg2: Block Device
# Arg3: Megabytes
biotest()
{
    if [ $BIOTEST_WEB_LOCATION = "skip" ] ; then
        debug "Skipping biotest test (TOOLS_ROOT = none)"
        return
    fi

    # Download the biotest binary
    debug_test "Download the biotest binary"
    run "$1 cd /root; /usr/bin/wget $BIOTEST_WEB_LOCATION &> /dev/null"
    if [ $RUN_RC -gt 0 ]; then
        debug_result 2
        return 0
    fi
    debug_result 0
    # Set file permissions
    debug_test "Set biotest permissions"
    run "$1 chmod 755 /root/biotest"
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi
    debug_result 0

    # CA-17375: check for existence of block device before running test
    check_vbd "$1" $2 15 || return 1

    # Run biotests
    # Buffered, in sequence
    debug_test "Biotest - Buffered, In Sequence"
    run "$1 /root/biotest -t $2 -m $3 -b"
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi
    debug_result 0
    # Direct, in sequence
    debug_test "Biotest - Direct, In Sequence"
    run "$1 /root/biotest -t $2 -m $3 -d"
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi
    debug_result 0
    # Buffered, random
    debug_test "Biotest - Buffered, Random"
    run "$1 /root/biotest -t $2 -m $3 -b -r"
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi
    debug_result 0
    # Direct, random
    debug_test "Biotest - Direct, Random"
    run "$1 /root/biotest -t $2 -m $3 -d -r"
    if [ $RUN_RC -gt 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    fi
    debug_result 0

    return 0
}

################################
## PERF TESTS
################################

# Arg1: block size
# Arg2: count
calc_megs()
{
    b=`echo "$1" | sed s/k/*1000/ | sed s/K/*1024/ | sed s/m/*100000/ | sed s/M/*1048576/`
    c=`echo "$2" | sed s/k/*1000/ | sed s/K/*1024/ | sed s/m/*100000/ | sed s/M/*1048576/`
    GLOBAL_RET=`echo "scale=2; $b * $c / (2^20)" | bc`
}

# Executes the dd command.
# TODO document arguments
# Arg1: prefix
# Arg2: input
# Arg3: output
# Arg4: BS
# Arg5: COUNT
dd_test()
{    
    debug_test "Running dd test"
    secs=`$1 time dd if=$2 of=$3 bs=$4 count=$5 2>&1`

    # TODO fix this up 
    RUN_OUTPUT=$secs
    RUN_LAST_CMD="$1 time dd if=$2 of=$3 bs=$4 count=$5 2>&1"
    RUN_RC=$?

    if [ $RUN_RC -ne 0 ]; then
        DD_RESULT="0"
        incr_exitval
        debug_result 1
        return 1
    else
        debug_result 0
    fi

    secs=`echo "$secs" | grep real | awk '{printf $2}'`
    secs=`echo "$secs" | perl -pe's/(\d+)m(\d+).(\d+)s/$1 $2.$3/g;'`
    secs=`echo "$secs" | awk '{print $1 * 60 + $2}'`
    calc_megs $4 $5
    megs=$GLOBAL_RET
    DD_RESULT=`echo "scale=2; $megs / $secs" | bc`
    return 0
}

# Executes a bonnie++ test and returns the output.
# TODO document arguments & bonnie parameters
# Arg1: name of device
# Arg2: prefix
# Arg3: directory/device
bonnie_test()
{
    debug_test "Running bonnie++ test"
    if [ -n "$FAST" ] ; then
        # fast bonnie run
        run $2 bonnie++ -m $1 -d $3 -u root -q -n 8:1024:8 -s 4 -r 2 2> /dev/null
    else
        run $2 bonnie++ -m $1 -d $3 -u root -q -n 64:1024:8 2> /dev/null
    fi

    results=$RUN_OUTPUT
    if [ $RUN_RC -ne 0 ]; then
        BONNIE_RESULT="0"
        incr_exitval
        debug_result 1
        return 1
    else
        debug_result 0
    fi

    # An example of bonnie's output is:
    #
    # Version 1.01d       ------Sequential Output------ --Sequential Input- --Random-
    #                     -Per Chr- --Block-- -Rewrite- -Per Chr- --Block-- --Seeks--
    # Machine        Size K/sec %CP K/sec %CP K/sec %CP K/sec %CP K/sec %CP  /sec %CP
    # xen-wim          7G 35346 10  35346  10 13569   0 35346 10  23991   0  74.8   0
    #                     ------Sequential Create------ --------Random Create--------
    #                     -Create-- --Read--- -Delete-- -Create-- --Read--- -Delete--
    # files:max:min        /sec %CP  /sec %CP  /sec %CP  /sec %CP  /sec %CP  /sec %CP
    #           64:1024:8  9968  39 66012  83 19146  37  9441  45 24647  30 27283  57
    # Dom0,7G,35346,10,35346,10,13569,0,35346,10,23991,0,74.8,0,64:1024:8,9968,39,66012,83,19146,37,9441,45,24647,30,27283,57
    #
    # return results, space delimeted of the final line only

    results=`echo "$results" | grep , | tr -d '\r' | perl -pe's/,/ /g;'`
    BONNIE_RESULT="$results"
    debug "Raw Bonnie results: ${BONNIE_RESULT}"
    return 0
}


################################
## FORMAT RESULTS
################################

# Arg1: log_file
# Arg2: graph_image_name
# Arg3: graph_title
gen_graph()
{
    tail -n30 $1 |                                                  \
        /usr/bin/ploticus -prefab chron unittype=datetime mode=line \
        datefmt=mm/dd/yy x=1 name="dom0" y=2 name2="dom0re"         \
        y2=3 name3="domU" y3=4 name4="domUre" y4=5 title="$3"       \
        ylbl="MB/sec" yrange=0 data=- -o $2 -png
}

# Arg1: log_file
# Arg2: graph_image_name ROOT
# Arg3: graph_title
bonnie_gen_graph()
{
#character IO output
    tail -n 30 $1 |                                                    \
        /usr/bin/ploticus -prefab chron unittype=datetime mode=line    \
        datefmt=mm/dd/yy x=1 y=4 name="dom0" y2=31 name2="blkback"    \
        title="$3 character output" ylbl="MB/sec" \
        yrange=0 data=- -o "$2"_char_out.png -png
#block IO output
    tail -n 30 $1 |                                                    \
        /usr/bin/ploticus -prefab chron unittype=datetime mode=line    \
        datefmt=mm/dd/yy x=1 y=6 name="dom0" y2=33 name2="blkback"    \
        title="$3 block output" ylbl="MB/sec"     \
        yrange=0 data=- -o "$2"_block_out.png -png
#rewrite IO output
    tail -n 30 $1 |                                                    \
        /usr/bin/ploticus -prefab chron unittype=datetime mode=line    \
        datefmt=mm/dd/yy x=1 y=8 name="dom0" y2=35 name2="blkback"    \
        title="$3 rewrite output" ylbl="MB/sec"   \
        yrange=0 data=- -o "$2"_rewrite_out.png -png
#character IO input
    tail -n 30 $1 |                                                    \
        /usr/bin/ploticus -prefab chron unittype=datetime mode=line    \
        datefmt=mm/dd/yy x=1 y=9 name="dom0" y2=36 name2="blkback"    \
        title="$3 character input" ylbl="MB/sec"  \
        yrange=0 data=- -o "$2"_char_in.png -png
#block IO input
    tail -n 30 $1 |                                                     \
        /usr/bin/ploticus -prefab chron unittype=datetime mode=line    \
        datefmt=mm/dd/yy x=1 y=11 name="dom0" y2=37 name2="blkback"   \
        title="$3 block input" ylbl="MB/sec"      \
        yrange=0 data=- -o "$2"_block_in.png -png
}

gen_html()
{
    if [ ! -e $WEB_DIR/dd_tests.html ]; then
        echo "<html><head><title>dd tests</title></head>" > $WEB_DIR/dd_tests.html
        echo "<body>" >> $WEB_DIR/dd_tests.html
        for mytype in "lvm" "ext" "nfs" "lvmoiscsi" ; do
            echo "<img src=\"${mytype}_reads.png\"><br>" >> $WEB_DIR/dd_tests.html
            echo "<img src=\"${mytype}_writes.png\"><br>" >> $WEB_DIR/dd_tests.html
        done
        echo "</body></html>" >> $WEB_DIR/dd_tests.html
        chmod 644 $WEB_DIR/dd_tests.html
    fi
}

# Arg1: image_file
publish_graph()
{
    gen_html
    chmod 644 $1    
    mv -f $1 $WEB_DIR
}

bonnie_gen_html()
{
    if [ ! -e $WEB_DIR/bonnie_tests.html ]; then
        echo "<html><head><title>bonnie++ tests</title></head>" > $WEB_DIR/bonnie_tests.html
        echo "<body><img src=\"bonnie_char_out.png\"><br>" >> $WEB_DIR/bonnie_tests.html
        echo "<body><img src=\"bonnie_block_out.png\"><br>" >> $WEB_DIR/bonnie_tests.html
        echo "<body><img src=\"bonnie_rewrite_out.png\"><br>" >> $WEB_DIR/bonnie_tests.html
        echo "<body><img src=\"bonnie_char_in.png\"><br>" >> $WEB_DIR/bonnie_tests.html
        echo "<body><img src=\"bonnie_block_out.png\"><br>" >> $WEB_DIR/bonnie_tests.html
        chmod 644 $WEB_DIR/bonnie_tests.html
    fi
}

# No args
bonnie_publish_graph()
{
    bonnie_gen_html
    chmod 644 $RESULTS_DIR/bonnie_char_out.png
    chmod 644 $RESULTS_DIR/bonnie_block_out.png
    chmod 644 $RESULTS_DIR/bonnie_rewrite_out.png
    chmod 644 $RESULTS_DIR/bonnie_char_in.png
    chmod 644 $RESULTS_DIR/bonnie_block_in.png
    mv -f $RESULTS_DIR/bonnie_char_out.png $WEB_DIR
    mv -f $RESULTS_DIR/bonnie_block_out.png $WEB_DIR
    mv -f $RESULTS_DIR/bonnie_rewrite_out.png $WEB_DIR
    mv -f $RESULTS_DIR/bonnie_char_in.png $WEB_DIR
    mv -f $RESULTS_DIR/bonnie_block_in.png $WEB_DIR
}

# Arg1: dom0 read
# Arg2: dom0 reread
# Arg3: dumU read
# Arg4: dumU reread
# Arg5: plot type
_plot()
{
    mkdir -p $RESULTS_DIR
    PLOT_TYPE="$5"
    RESULTS_FILE="${RESULTS_DIR}/${SUBSTRATE_TYPE}_${PLOT_TYPE}.txt"
    RESULTS_PNG="${RESULTS_DIR}/${SUBSTRATE_TYPE}_${PLOT_TYPE}.png"
    date=`date +%D`
    if [ ! -e ${RESULTS_FILE} ]; then
        ## graphs for one day or less are illegible
        hack=`date -dyesterday +%D`
        echo "$hack $1 $2 $3 $4" >> ${RESULTS_FILE}
    fi
    echo "$date $1 $2 $3 $4" >> ${RESULTS_FILE}
    debug_test "Generating dd graphs"
    if [ -e /usr/bin/ploticus ]; then
        gen_graph ${RESULTS_FILE} ${RESULTS_PNG} "${SUBSTRATE_TYPE}_${PLOT_TYPE}"
        publish_graph ${RESULTS_PNG}
        debug_result 0
    else 
        debug_result 2
    fi
}


# Arg1: SUBSTRATE_TYPE
# Arg2: dom0 read
# Arg3: dom0 reread
# Arg4: dumU read
# Arg5: dumU reread
plot_reads()
{
    _plot $2 $3 $4 $5 "reads"
}

# Arg1: dom0
# Arg2: dom0 write
# Arg3: dom0 rewrite
# Arg4: dumU write
# Arg5: dumU rewrite
plot_writes()
{
    _plot $2 $3 $4 $5 "writes"
}

# Arg1: dom0 data
# Arg2: blkback data
bonnie_graph_data()
{
    # append results together, separated by comma's for ploticus
    mkdir -p $RESULTS_DIR
    date=`date +%D`
    if [ ! -e $RESULTS_DIR/bonnie.txt ]; then
        ## graphs for one day or less are illegible
        hack=`date -dyesterday +%D`
        echo "$hack $1 $2" >> $RESULTS_DIR/bonnie.txt
    fi
    echo "$date $1 $2" >> $RESULTS_DIR/bonnie.txt

    debug_test "Generating Bonnie++ graphs"
    # if ploticus installed then plot otherwise skip
    if [ -e /usr/bin/ploticus ]; then
        bonnie_gen_graph $RESULTS_DIR/bonnie.txt $RESULTS_DIR/bonnie bonnie
        bonnie_publish_graph
        debug_result 0
    else 
        #incr_exitval
        debug_result 2
    fi
}

# $1 is REM_CMD
# $2 is LOGFILE to transfer
tx_fsx_log() 
{
    SCP_PREFIX=`echo "$1" | sed "s/ssh /scp /"`

    FSX_DATE=`date +%s`
    OUT_DIR="/tmp/fsxlog.$FSX_DATE/"

    SCP_CMD="$SCP_PREFIX:$2 $OUT_DIR"

    mkdir -p $OUT_DIR
    output=`$SCP_CMD`

    FSX_LOG_CONTENT=`cat $OUT_DIR/fsxlog`
    debug "FSX log output:
    
$FSX_LOG_CONTENT

"

    rm -rf "$OUT_DIR"

    return
}

# Performs a data integrity test: creates a FS an executes FSX on it.
# $1 is REM_CMD
# $2 is DEV_NAME
# $3 is NUM_ITERATIONS
run_fsx() 
{
    if [ $FSX_WEB_LOCATION = "skip" ] ; then
        debug "Skipping FSX test (TOOLS_ROOT = none)"
        return
    fi

    MNTPNT="/tmp/stressmnt"
    TESTFILE="${MNTPNT}/testfile"
    LOGFILE="${TESTFILE}.fsxlog"
    debug_test "Running data integrity (fsx) test"

    #set up
    run "$1 mkfs -t ext3 $2"
    if [ $? -ne 0 ] ; then debug "mkfs failed" ; debug_result 1; incr_exitval ; return 1; fi

    run "$1 mkdir -p ${MNTPNT}"
    if [ $? -ne 0 ] ; then debug "mkdir failed" ; debug_result 1; incr_exitval ; return 1; fi

    # run
    run "$1 /usr/bin/wget $FSX_WEB_LOCATION &> /dev/null"
    if [ $? -ne 0 ] ; then debug "wget failed" ; debug_result 2; return 0; fi

    run "$1 mount $2 ${MNTPNT}"
    if [ $? -ne 0 ] ; then debug "mount failed" ; debug_result 1; incr_exitval ; return 1; fi

    run "$1 chmod +x ./fsx"
    if [ $? -ne 0 ] ; then debug "chmod failed" ; debug_result 1; incr_exitval ; return 1; fi

    run "$1 ./fsx -c 200 -l 2048000 -N $3 ${MNTPNT}/testfile"
    if [ $? -ne 0 ] ; then debug "fsx failed" ; debug_result 1; incr_exitval ; return 1; fi

    run "$1 cp $LOGFILE /tmp/fsxlog"
    if [ $? -ne 0 ] ; then debug "cp failed" ; debug_result 1; incr_exitval ; return 1; fi

    # clean up

    `$1 umount ${MNTPNT}`
    `$1 rmdir ${MNTPNT}`

    if [ $RUN_RC -ne 0 ]; then
        debug_result 1
        incr_exitval
        tx_fsx_log "$1" "/tmp/fsxlog"
        return 1
    else
        #uncomment if you want to see fsx output always
        #tx_fsx_log "$1" "/tmp/fsxlog"
        debug_result 0
    fi
}

# $1 is REM_CMD
# $2 is DEV_NAME
# $3 is NUM_ITERATIONS
run_postmark() 
{
    if [ $PMARK_BINARY_LOCATION = "skip" ] ; then
        debug "Skipping postmark test (TOOLS_ROOT = none)"
        return
    fi

    MNTPNT="/tmp/postmarkmnt"
    debug_test "Running postmark test"

    #set up. Disk is already formatted and ready to mount
    run "$1 mkdir -p ${MNTPNT}"
    run "$1 mount $2 ${MNTPNT}"

    # setup postmark
    run "$1 cd ${MNTPNT}; /usr/bin/wget $PMARK_BINARY_LOCATION &> /dev/null"
    if [ $? -ne 0 ] ; then debug "wget failed" ; debug_result 2; run "$1 umount $MNTPNT"; return 0; fi

    run "$1 cd ${MNTPNT}; chmod +x ./postmark"
    if [ ! -n $FAST ] ; then
        FAST_FLAG=""
    else
        FAST_FLAG=".fast"
    fi
    run "$1 cd ${MNTPNT}; /usr/bin/wget $PMARK_COMMANDS_LOCATION$FAST_FLAG &> /dev/null"
    if [ $? -ne 0 ] ; then debug "wget failed" ; debug_result 2; run "$1 umount $MNTPNT"; return 0; fi

    # run
    run "$1 cd ${MNTPNT}; ./postmark stress_test$FAST_FLAG"

    PMARK_RC=${RUN_RC}

    # clean up

    run "$1 umount ${MNTPNT}"
    run "$1 rmdir ${MNTPNT}"

    if [ $PMARK_RC -ne 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
    fi
}

# $1 is REM_CMD
# $2 is DEV_NAME
run_mkfs()
{
    debug_test "Inserting FS on disk"
    test_device="/dev/$2"

    run "$1 mkfs.ext3 -F ${test_device} &> /dev/null"
    if [ $RUN_RC -ne 0 ]; then
        debug_result 1
        incr_exitval
	exit
        return 1
    else
        debug_result 0
    fi
}

# $1 is REM_CMD
# $2 is DEV_NAME
run_fsck()
{
    debug_test "Verifying FS integrity (fsck)"
    test_device="/dev/$2"

    run "$1 fsck -n ${test_device} &> /dev/null"
    if [ $RUN_RC -ne 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
    fi
}

# FIXME redundant, see zero_disk or similar
# $1 is REM_CMD
# $2 is DEV_NAME
run_diskwipe()
{
    debug_test "Wiping start of disk"
    test_device="/dev/$2"

    run "$1 dd if=/dev/zero of=${test_device} bs=1M count=10 &> /dev/null"
    if [ $RUN_RC -ne 0 ]; then
        debug_result 1
        incr_exitval
        return 1
    else
        debug_result 0
    fi
}
