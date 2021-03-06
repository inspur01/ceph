#!/bin/sh -x

set -e

git clone https://github.com/inspur01/xfstests.git
make -C xfstests
cp xfstests/ltp/fsx .

OPTIONS="-z"  # don't use zero range calls; not supported by cephfs

./fsx $OPTIONS  1MB -N 50000 -p 10000 -l 1048576
./fsx $OPTIONS  10MB -N 50000 -p 10000 -l 10485760
./fsx $OPTIONS 100MB -N 50000 -p 10000 -l 104857600
