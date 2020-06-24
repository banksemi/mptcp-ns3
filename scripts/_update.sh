#! /bin/bash

function CodeCopy() {
	cp '../dce/source/'$1 '../../source/'$1'/../' -R -f
	echo Copy: '../dce/source/'$1 '../../source/'$1'/../' -R -f
}

CodeCopy "net-next-nuse-4.4.0"
CodeCopy "ns-3-dce/myscripts"