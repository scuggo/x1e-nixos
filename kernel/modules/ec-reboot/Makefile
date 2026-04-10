ifneq ($(KERNELRELEASE),)
obj-m += ec-reboot.o
ec-reboot-objs := ec_reboot.o
else
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
endif
