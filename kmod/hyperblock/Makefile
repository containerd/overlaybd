obj-$(CONFIG_HYPERBLOCK) += loop.o 
loop-objs := src/loop.o src/lsmt_ro_file.o
KERNEL_PATH=/home/zgl/vagrant/hyperblock_loop
all:
	make -C $(KERNEL_PATH) M=$(PWD) modules

clean:
	make -C $(KERNEL_PATH) M=$(PWD) clean
