./_update.sh
# ./linux-build.sh
./iperf3-build.sh

rm ../logs/*.txt
rm ../logs_receiver/*.txt

rm ../*.pcap

mkdir ../../pcap/logs
mkdir ../../pcap/logs_receiver



./dce-run.sh "test-standard  --sched=default --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/test-standard-default.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/test-standard-default.txt

./dce-run.sh "test-standard  --sched=blest --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/test-standard-blest.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/test-standard-blest.txt

./dce-run.sh "test-standard  --sched=redundant --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/test-standard-redundant.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/test-standard-redundant.txt

./dce-run.sh "test-standard --sched=only_fast --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/test-standard-only_fast.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/test-standard-only_fast.txt




./dce-run.sh "test-standard-new  --sched=default --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/test-standard-new-default.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/test-standard-new-default.txt

./dce-run.sh "test-standard-new  --sched=blest --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/test-standard-new-blest.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/test-standard-new-blest.txt

./dce-run.sh "test-standard-new  --sched=redundant --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/test-standard-new-redundant.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/test-standard-new-redundant.txt

./dce-run.sh "test-standard-new --sched=only_fast --bandwidth=1Mbit"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/test-standard-new-only_fast.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/test-standard-new-only_fast.txt

