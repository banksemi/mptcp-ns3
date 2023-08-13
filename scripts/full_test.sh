./_update.sh
./linux-build.sh
./iperf3-build.sh

rm ../logs/*.txt
rm ../logs_receiver/*.txt

rm ../*.pcap

mkdir ../../pcap/logs
mkdir ../../pcap/logs_receiver

./dce-run.sh "test-standard --sched=only_fast --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/RTT강제변경_only_fast.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/RTT강제변경_only_fast.txt
