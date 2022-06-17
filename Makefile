KERNELDIR = /usr/src/linux
#CFLAGS = -D__KERNEL__ -DMODULE -I$(KERNELDIR)/include -O
EXTRA_CFLAGS = -Wall #-DCONFIG_SND_DEBUG

ifneq ($(KERNELRELEASE),)

obj-m	:= motu.o

else

KDIR	:= /lib/modules/$(shell uname -r)/build
PWD	:= $(shell pwd)

default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules
endif

clean:
	rm -f *.[oas] *.ko *.mod.c modules.* Module.*
