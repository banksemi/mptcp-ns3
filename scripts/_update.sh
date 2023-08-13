#! /bin/bash

function CodeCopy() {
	cp '../dce/source/'$1 '../../source/'$1'/../' -R -f
	echo Copy: '../dce/source/'$1 '../../source/'$1'/../' -R -f
}

CodeCopy "iperf3"
CodeCopy "ns-3.28/src/mmwave/model"
# CodeCopy "net-next-nuse-4.4.0/include"
# CodeCopy "net-next-nuse-4.4.0/net/ipv4"
CodeCopy "net-next-nuse-4.4.0/net/mptcp"
CodeCopy "ns-3-dce/myscripts"