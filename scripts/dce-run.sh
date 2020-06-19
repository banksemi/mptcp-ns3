#! /bin/bash

# 커널 폴더로
cd ../../

mkdir pcap
rm ./pcap/*.pcap

cd ./source/ns-3-dce

./waf --run test-standard

