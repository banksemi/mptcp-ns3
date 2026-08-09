congestion_control="cubic"  # cubic, reno, lia, balia
guard_latency_us=0           # microseconds
use_three_paths=true         # false: 2 paths, true: 3 paths

path_count=2
if [ "$use_three_paths" = true ]; then
    path_count=3
fi

experiment_options="--congestionControl=$congestion_control --guardLatencyUs=$guard_latency_us --pathCount=$path_count"

./_update.sh
./linux-build.sh

rm ../logs/*.txt
rm ../logs_receiver/*.txt

rm ../*.pcap
mkdir ../../pcap/logs
mkdir ../../pcap/logs_receiver

./dce-run.sh "test-standard --sched=default --bandwidth=1Mbit $experiment_options"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/default.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/default.txt

./dce-run.sh "test-standard --sched=blest --bandwidth=1Mbit $experiment_options"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/blest.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/blest.txt

./dce-run.sh "test-standard --sched=redundant --bandwidth=1Mbit $experiment_options"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/redundant.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/redundant.txt

./dce-run.sh "test-standard --sched=only_fast --bandwidth=1Mbit $experiment_options"
cp ../../source/ns-3-dce/files-0/var/log/messages ../../pcap/logs_receiver/only_fast.txt
cp ../../source/ns-3-dce/files-1/var/log/messages ../../pcap/logs/only_fast.txt
