# create mount directory
mkdir -p /mnt/hyperblock
# mount loop device to target directory
mount -o ro /dev/loop0 /mnt/hyperblock/
