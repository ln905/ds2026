#!/bin/bash
MOUNT_POINT="/home/kali/Downloads/gluster_data"
TEST_FILE="$MOUNT_POINT/test_1gb.dat"

echo "   GLUSTERFS BENCHMARK: LARGE FILE I/O"

if [ ! -d "$MOUNT_POINT" ]; then
    echo "Error: Mount point $MOUNT_POINT does not exist."
    exit 1
fi

echo "Starting WRITE benchmark (1GB file)..."
dd if=/dev/zero of=$TEST_FILE bs=1M count=1024 oflag=direct conv=fdatasync status=progress
echo "Write test completed."

echo "Clearing system cache to ensure accurate READ test..."
sudo sh -c "sync; echo 3 > /proc/sys/vm/drop_caches"

echo "Starting READ benchmark (1GB file)..."
dd if=$TEST_FILE of=/dev/null bs=1M status=progress
echo "Read test completed."

rm -f $TEST_FILE
echo "Cleanup done. Benchmark finished."
