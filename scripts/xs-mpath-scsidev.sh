#!/bin/bash

BUS=${1}
NAME=${2}
HOST=${BUS%%:*}
NODE=${3}
OUT=${4}
LUN=`echo $BUS | cut -d: -f4`

# The required files are located in an extra layer of directories in 3.x kernels
kernel=`uname -r | cut -d. -f1-2`
case $kernel in
    2.6)
        targetIQNPath="/device/session*/iscsi_session*"
        targetIPPortPath="/device/session*/connection*/iscsi_connection*"
        ;;
    3.*)
        targetIQNPath="/device/session*/iscsi_session/session*"
        targetIPPortPath="/device/session*/connection*/iscsi_connection/connection*"
        ;;
    *)
        echo "Unkown kernel: neither 2.6 nor 3.x. Exiting..."
        exit 2
        ;;
esac

#Args: ID
query_hosts() {
    /usr/sbin/mppUtil -g $1 | grep hostId | while read file
      do
      host=`echo $file | awk '{gsub(/^.*\(|\).*$/,"");gsub(/,/,"");print $2}'`
      ISCSIPATH="/sys/class/iscsi_host/host${host}"
      [ -e ${ISCSIPATH} ] || continue

      #Query the IQN, IP and Port values
      targetIQNfile="${ISCSIPATH}${targetIQNPath}/targetname"
      targetIQN=$(cat ${targetIQNfile})

      targetIPfile="${ISCSIPATH}${targetIPPortPath}/persistent_address"
      targetPortfile="${ISCSIPATH}${targetIPPortPath}/persistent_port"
      targetIP=$(cat ${targetIPfile})
      targetPort=$(cat ${targetPortfile})

      path="/dev/iscsi/${targetIQN}/${targetIP}:${targetPort}/SESSIONID-${host}"
      echo "${host}"
      return
      done
}

if [ $# -gt 5 -o $# -lt 4 ]; then
        echo "Incorrect number of arguments"
        exit 1
fi

if [[ $OUT == "scsi_bybus" && -z ${5} ]]; then
    SCSIID=`/usr/lib/udev/scsi_id --whitelisted --replace-whitespace --device=$NODE`
    if [ $? == 0 ]; then
        echo "disk/by-scsibus/${SCSIID}-${BUS} disk/by-scsid/${SCSIID}/$2"
    fi
    exit
fi

[ -e /sys/class/scsi_host/host${HOST}/proc_name ] || exit 1

driver=$(cat /sys/class/scsi_host/host${HOST}/proc_name)

if [ $driver == "mpp" ]; then
    # RDAC special device handling
    array=`/opt/xensource/bin/xe-getarrayidentifier ${NODE}`
    id=`/usr/sbin/mppUtil -a | grep ${array} | awk '{print $1}'`
    if [[ $OUT == "scsi_bympp" && -z ${5} ]]; then
        SCSIID=`/usr/lib/udev/scsi_id --whitelisted --replace-whitespace --device=$NODE`
        if [ $? == 0 ]; then
            #lunnum=`/opt/xensource/bin/xe-getlunidentifier ${NODE}`
            #lunid=`/usr/sbin/mppUtil -g ${id} | grep ${lunnum} | awk '{gsub(/#/,"");print $2}'`
            #echo "disk/by-mpp/${SCSIID}-${id}:${lunid}"
            echo "disk/by-mpp/${SCSIID}"
        fi
        exit
    else
        HOST=`query_hosts $id`
    fi
fi

ISCSIPATH="/sys/class/iscsi_host/host${HOST}"

[ -e ${ISCSIPATH} ] || exit 1

# Query targetIQN
targetIQNfile="${ISCSIPATH}${targetIQNPath}/targetname"
targetIQN=$(cat ${targetIQNfile})

# Query target address and port
targetIPfile="${ISCSIPATH}${targetIPPortPath}/persistent_address"
targetPortfile="${ISCSIPATH}${targetIPPortPath}/persistent_port"
targetIP=$(cat ${targetIPfile})
targetPort=$(cat ${targetPortfile})

basepath="iscsi/${targetIQN}/${targetIP}:${targetPort}"

# Handle the Kernel assigned device number
if [ -z ${5} ]; then
   DEV="LUN${LUN}"
else
   DEV="LUN${LUN}_${5}"
fi

if [ ${OUT} == "base" ]; then
    echo "${basepath}/${DEV}"
elif [ ${OUT} == "session" ]; then
    echo "${basepath}/SESSIONID-${HOST}/${DEV}"
elif [[ ${OUT} == "serial" && -z ${5} ]]; then
    SERIAL=`/opt/xensource/bin/xe-getserialhex ${NODE}`
    if [ $? == 0 ]; then
	echo "${basepath}/SERIAL-${SERIAL}"
    fi
fi
