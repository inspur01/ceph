#!/bin/sh -x

set -e

wget http://teuthology.inspur.com/ceph/other/fsync-tester.c
gcc fsync-tester.c -o fsync-tester

./fsync-tester

echo $PATH
whereis lsof
lsof
