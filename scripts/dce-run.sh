#! /bin/bash

# 커널 폴더로
cd ../../

mkdir pcap
rm ./pcap/pcap_file*.pcap
rm ./pcap/messages

cd ./source/ns-3-dce

if [ $# -ne 1 ]; then
    ./waf --run mmwave --command-template="gdb %s"
else
    ./waf --run "$@"
fi

cp ./files-1/var/log/messages ../../pcap/messages
# --command-template="gdb %s"
