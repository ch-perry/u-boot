obj-y	+= rpmi-uclass.o
ifeq ($(CONFIG_$(PHASE_)RISCV_SMODE),y)
obj-$(CONFIG_RPMI_SBI_MPXY) += rpmi-sbi-mpxy.o
endif
obj-$(CONFIG_RPMI_SHMEM) += rpmi-shmem.o
obj-$(CONFIG_SANDBOX)	+= rpmi-sandbox.o rpmi-sandbox-clock.o
