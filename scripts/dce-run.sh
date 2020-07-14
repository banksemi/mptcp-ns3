#! /bin/bash

# 커널 폴더로
cd ../../

mkdir pcap
rm ./pcap/*.pcap

cd ./source/ns-3-dce

if [ $# -ne 1 ]; then
    ./waf --run test-standard
else
    ./waf --run "$@"
fi
# --command-template="gdb %s"
