./_update.sh
./linux-build.sh


rm ../logs/*.txt


./dce-run.sh "test-standard --sched=default"
cp ../../source/ns-3-dce/files-1/var/log/messages ../logs/default.txt

./dce-run.sh "test-standard --sched=blest"
cp ../../source/ns-3-dce/files-1/var/log/messages ../logs/blest.txt

./dce-run.sh "test-standard --sched=redundant"
cp ../../source/ns-3-dce/files-1/var/log/messages ../logs/redundant.txt