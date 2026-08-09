# make clean 
# 클린을 해주지 않아도 일반적인 경우 작동하지만, 실제 커널이 업데이트 되지 않는 경우가 있음.
# clean으로 인해 작업이 오래 걸리지만 -> 수정 내역을 잘 반영시키기 위해 사용

# 커널 폴더로
cd ../../source/net-next-nuse-4.4.0
make clean


# /dce/source/net-next-nuse-4.4.0 -> /dce
cd ../../
if ! bake.py build; then
    echo "bake.py build failed; continuing with the authoritative kernel library build" >&2
fi

# /dce -> /dce/source/net-next-nuse-4.4.0
cd ./source/net-next-nuse-4.4.0
if ! make library ARCH=lib; then
    echo "Kernel library build failed" >&2
    exit 1
fi

rm -f liblinux.so
ln -s ./arch/lib/tools/libsim-linux.so ./liblinux.so

# /dce/source/net-next-nuse-4.4.0 -> /dce
cd ../../

# /dce -> /dce/source/iperf3
cd ./source/iperf3
make clean
if ! make; then
    echo "iperf3 build failed" >&2
    exit 1
fi
