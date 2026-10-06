ifneq ($(KERNELRELEASE),)

# 被内核构建系统调用时，只保留内核模块描述。
obj-m += camera_timer.o
obj-m += camera_key.o

else

CC := /home/lzy/linux/tool/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-gcc
APP_CFLAGS ?= -O2 -Wall -Wextra -std=c99
LDLIBS ?= -pthread
KDIR ?= /home/lzy/linux/uboot/linux_nxp
KERNEL_CROSS_COMPILE ?= /home/lzy/linux/tool/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-

APP := camera
APP_OBJECTS := camera.o camera_capture.o camera_save.o

.PHONY: all timer_module clean

all: $(APP) timer_module

$(APP): $(APP_OBJECTS)
	$(CC) $(APP_CFLAGS) -o $@ $^ $(LDLIBS)

camera.o: camera.c camera.h camera_timer_events.h
	$(CC) $(APP_CFLAGS) -c -o $@ $<

camera_capture.o: camera_capture.c camera.h
	$(CC) $(APP_CFLAGS) -c -o $@ $<

camera_save.o: camera_save.c camera.h camera_timer_events.h
	$(CC) $(APP_CFLAGS) -c -o $@ $<

timer_module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=$(KERNEL_CROSS_COMPILE) modules

clean:
	rm -f $(APP) $(APP_OBJECTS)
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm CROSS_COMPILE=$(KERNEL_CROSS_COMPILE) clean

endif
