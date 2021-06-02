# Uncompress test file to tmp directory
LSMT_PATH=/mnt/hgfs/vagrant/scripts
cd $LSMT_PATH;
tar zxvf test_img.tgz -C /tmp/

# Replace loop with modified kernel module
HB_PATH=/mnt/hgfs/vagrant/hyperblock_loop
cd $HB_PATH;
rmmod loop -f;
insmod drivers/block/hyperblock/loop.ko 

# steps to help debug
OLD_PATH=/mnt/hgfs/vagrant/
cd $OLD_PATH;
cat /sys/module/loop/sections/.init.text > tmp

