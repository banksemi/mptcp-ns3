#! /bin/bash

# NS3 관련 파일들을 git에서 복사
./_update.sh

# MPTCP 설정
cd ../../source/net-next-nuse-4.4.0
cat >> arch/lib/defconfig <<END
CONFIG_MPTCP=y
CONFIG_MPTCP_PM_ADVANCED=y
CONFIG_MPTCP_FULLMESH=y
CONFIG_MPTCP_NDIFFPORTS=y
CONFIG_DEFAULT_FULLMESH=y
CONFIG_DEFAULT_MPTCP_PM="fullmesh"
CONFIG_MPTCP_SCHED_ADVANCED=y
CONFIG_MPTCP_ROUNDROBIN=y
CONFIG_DEFAULT_MPTCP_SCHED="default"
END

make defconfig ARCH=lib

# 커널 빌드
cd ../../mptcp-ns3/scripts
./linux-build.sh