./_update.sh
./linux-build.sh
./iperf3-build.sh

rm ../logs/*.txt
rm ../logs_receiver/*.txt

rm ../*.pcap
mkdir ../../pcap/logs
mkdir ../../pcap/logs_receiver

./dce-run.sh "test-standard --sched=default --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/default.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/default.txt

./dce-run.sh "test-standard --sched=blest --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/blest.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/blest.txt

./dce-run.sh "test-standard --sched=redundant --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/redundant.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/redundant.txt

./dce-run.sh "test-standard --sched=only_fast --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/only_fast.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/only_fast.txt
