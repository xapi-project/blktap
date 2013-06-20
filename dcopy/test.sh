#!/bin/bash
#
# test dcopy


do_test ()
{
        chunk=$1
        iter=$2
        pad=$3
        opt=$4

        echo -ne "${chunk}KB chunk * 2 * $iter iterations. pad=$padB "
        echo -ne "(opt:$opt)  "
        rm -f source_img dest_img
        ./tp source_img $chunk $iter $pad
        ./dcopy $opt source_img dest_img
        diff source_img dest_img >> /dev/null
        if [ $? -eq 0 ]; then
             echo "[ PASS ]"
        else 
             echo "[ FAIL ]"
        fi
        rm -f source_img dest_img
}

do_test 1 1024 0 ""
do_test 1 1024 0 "--sparse"
do_test 1 8192 0 ""
do_test 1 8192 0 "--sparse"

do_test 1 1024 5 ""
do_test 1 1024 5 "--sparse"


do_test 128 128 5 ""
do_test 128 128 5 "--sparse"
