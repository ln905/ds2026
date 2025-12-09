#!/bin/bash
echo "STARTING GLUSTERFS SETUP"

echo "Updating repositories and installing GlusterFS"
sudo apt-get update -qq
sudo apt-get install -y glusterfs-server

echo "Enabling and starting Glusterd service"
sudo systemctl enable --now glusterd
sudo systemctl status glusterd --no-pager | grep "Active"

BRICK_DIR="/data/glusterfs/myvolume/brick1"
echo "Creating brick directory at: $BRICK_DIR"
sudo mkdir -p $BRICK_DIR

echo "SETUP COMPLETED"

