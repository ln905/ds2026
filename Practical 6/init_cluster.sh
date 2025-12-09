#!/bin/bash
SERVER2_IP="192.168.126.138"
VOL_NAME="gv0"
BRICK_PATH="/data/glusterfs/myvolume/brick1"

echo "STARTING CLUSTER INITIALIZATION"

echo "Probing peer Server 2 ($SERVER2_IP)"
sudo gluster peer probe $SERVER2_IP

sleep 2

echo "Current Peer Status:"
sudo gluster peer status

echo "Creating Volume '$VOL_NAME' (Replica Count: 2)"
sudo gluster volume create $VOL_NAME replica 2 \
    $(hostname):$BRICK_PATH \
    $SERVER2_IP:$BRICK_PATH \
    force

echo "Starting Volume"
sudo gluster volume start $VOL_NAME

echo "SUCCESS! VOLUME DETAILS:"
sudo gluster volume info
