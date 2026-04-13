obj-m += YAH.o
YAH-y := YetAnotherHypervisor.o virt.o
CFLAGS_REMOVE_YetAnotherHypervisor.o += -pg -mrecord-mcount -mfentry -fpatchable-function-entry=16,16 -fstack-protector-strong
CFLAGS_YetAnotherHypervisor.o += -fno-stack-protector
OBJECT_FILES_NON_STANDARD_virt.o := y
all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
