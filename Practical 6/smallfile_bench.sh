#!/bin/bash
MOUNT_POINT="/home/kali/Downloads/gluster_data"
SMALL_FILES_DIR="$MOUNT_POINT/small_files"
COUNT=1000

echo "   GLUSTERFS BENCHMARK: METADATA OPERATIONS"

if [ ! -d "$MOUNT_POINT" ]; then
    echo "Error: Mount point $MOUNT_POINT does not exist."
    exit 1
fi

mkdir -p $SMALL_FILES_DIR

echo "Creating $COUNT empty files"
START_TIME=$(date +%s%N)

for ((i=1; i<=COUNT; i++)); do
    touch "$SMALL_FILES_DIR/file_$i"
done

END_TIME=$(date +%s%N)
ELAPSED=$(( (END_TIME - START_TIME) / 1000000 )) #milliseconds
if [ $ELAPSED -eq 0 ]; then ELAPSED=1; fi
TPS=$(( COUNT * 1000 / ELAPSED ))

echo "	Time elapsed: ${ELAPSED} ms"
echo "	Creation Rate: $TPS files/second"

echo "Deleting $COUNT files"
START_TIME=$(date +%s%N)

rm -rf "$SMALL_FILES_DIR"/*

END_TIME=$(date +%s%N)
ELAPSED=$(( (END_TIME - START_TIME) / 1000000 ))
if [ $ELAPSED -eq 0 ]; then ELAPSED=1; fi
TPS=$(( COUNT * 1000 / ELAPSED ))

echo "	Time elapsed: ${ELAPSED} ms"
echo "	Deletion Rate: $TPS files/second"

rmdir $SMALL_FILES_DIR
echo "Benchmark finished."
