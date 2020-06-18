#! /bin/bash

function CodeCopy() {
	cp '../dce/source/'$1 '../../source/'$1 -R
	echo "Copy:" $1
}
git pull

CodeCopy "net-next-nuse-4.4.0/net"
CodeCopy "ns-3-dce/myscripts"