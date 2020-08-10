function Test() {
    ./dce-run.sh "test-standard --bandwidth=$2 --sched=$3"
    cp ../../source/ns-3-dce/files-1/var/log/messages ../logs/$1_$3.txt
    cp ../../pcap/pcap_file-1-0.pcap ../logs/$1_$3_interface1.pcap
    cp ../../pcap/pcap_file-1-1.pcap ../logs/$1_$3_interface2.pcap
}

rm ../logs/*.txt
rm ../logs/*.pcap



Test "iperf2bandwidth" "5Mbit" "default";
Test "iperf2bandwidth" "5Mbit" "blest";
Test "iperf2bandwidth" "5Mbit" "redundant";
Test "iperf2bandwidth" "5Mbit" "only_fast";
