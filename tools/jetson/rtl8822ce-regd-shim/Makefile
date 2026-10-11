# SPDX-License-Identifier: GPL-2.0
# Out-of-tree kbuild for rtl8822ce-regd-shim.
#
# Build (on the Jetson):
#   make -C /lib/modules/$(uname -r)/build M=/tmp/opencode/regd-shim modules
obj-m += rtl8822ce-regd-shim.o

ccflags-y += -Wall

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean

.PHONY: all clean
