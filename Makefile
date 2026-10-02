# ============================================================
# IMX6ULL SDK 顶层编排 Makefile —— 在 build/ 下运行
#   make menuconfig   配置 SDK
#   make              全量构建（uboot→kernel→rootfs→sdcard.img）
#   make uboot / kernel / rootfs / images   单独构建某环
#   make clean        清空 output/
# ============================================================
SDK_ROOT  := $(abspath ..)
OUT       := $(SDK_ROOT)/output
IMAGES    := $(OUT)/images
KC        := scripts/kconfig

-include .config                       # 载入 menuconfig 结果
quote    = $(subst ",,$(1))
CROSS    := $(SDK_ROOT)/$(call quote,$(CONFIG_SDK_CROSS_PREFIX))
TC_ROOT  := $(patsubst %/,%,$(dir $(CROSS)))..        # 工具链根目录
JOBS     := $(CONFIG_SDK_JOBS)
DTB      := $(call quote,$(CONFIG_SDK_KERNEL_DTB))
BOARD    := board/$(call quote,$(CONFIG_SDK_BOARD_NAME))
UBOOT_DEF   := mx6ull_alientek_emmc_defconfig
KERNEL_DEF  := imx6ull-alientek-emmc_defconfig

.PHONY: all menuconfig defconfig oldconfig savedefconfig \
        check uboot kernel rootfs images clean

all: check uboot kernel rootfs images

check:
ifndef CONFIG_SDK_BOARD_NAME
	$(error 先运行 make defconfig 或 make menuconfig)
endif
	@test -x $(CROSS)gcc || { echo "工具链不存在: $(CROSS)gcc"; exit 1; }
	@mkdir -p $(OUT)/images $(OUT)/modules

# ---------- SDK 自身配置（kconfig） ----------
$(KC)/conf $(KC)/mconf:
	$(MAKE) -C $(KC)

menuconfig: $(KC)/mconf
	$(KC)/mconf Kconfig
	@echo "配置已存到 .config，可 make savedefconfig 固化到板级目录"

defconfig: $(KC)/conf
	$(KC)/conf --defconfig=$(BOARD)/sdk_defconfig Kconfig

oldconfig: $(KC)/conf
	$(KC)/conf Kconfig

savedefconfig: $(KC)/conf
	$(KC)/conf --savedefconfig=$(BOARD)/sdk_defconfig Kconfig

# ---------- U-Boot ----------
uboot:
ifeq ($(CONFIG_SDK_BUILD_UBOOT),y)
	$(MAKE) -C $(SDK_ROOT)/uboot O=$(OUT)/uboot CROSS_COMPILE=$(CROSS) $(UBOOT_DEF)
	$(MAKE) -C $(SDK_ROOT)/uboot O=$(OUT)/uboot CROSS_COMPILE=$(CROSS) -j$(JOBS)
	cp $(OUT)/uboot/u-boot-dtb.imx $(IMAGES)/
endif

# ---------- 内核 ----------
kernel:
ifeq ($(CONFIG_SDK_BUILD_KERNEL),y)
	$(MAKE) -C $(SDK_ROOT)/kernel O=$(OUT)/kernel ARCH=arm CROSS_COMPILE=$(CROSS) $(KERNEL_DEF)
	$(MAKE) -C $(SDK_ROOT)/kernel O=$(OUT)/kernel ARCH=arm CROSS_COMPILE=$(CROSS) -j$(JOBS) \
		zImage dtbs modules
ifeq ($(CONFIG_SDK_INSTALL_MODULES),y)
	$(MAKE) -C $(SDK_ROOT)/kernel O=$(OUT)/kernel ARCH=arm CROSS_COMPILE=$(CROSS) \
		modules_install INSTALL_MOD_PATH=$(OUT)/modules
endif
	cp $(OUT)/kernel/arch/arm/boot/zImage $(IMAGES)/
	cp $(OUT)/kernel/arch/arm/boot/dts/$(DTB).dtb $(IMAGES)/
endif

# ---------- rootfs（buildroot 只做 rootfs，内核/uboot 由上面统一编） ----------
$(OUT)/rootfs/defconfig: $(BOARD)/buildroot_defconfig
	@mkdir -p $(OUT)/rootfs
	sed -e 's|@SDK_TOOLS@|$(TC_ROOT)|g' -e 's|@BOARD_DIR@|$(realpath $(BOARD))|g' $< > $@

rootfs: $(OUT)/rootfs/defconfig
ifeq ($(CONFIG_SDK_BUILD_ROOTFS),y)
	$(MAKE) -C $(SDK_ROOT)/buildroot O=$(OUT)/rootfs BR2_DEFCONFIG=$(OUT)/rootfs/defconfig defconfig
	$(MAKE) -C $(SDK_ROOT)/buildroot O=$(OUT)/rootfs SDK_MODULES_DIR=$(OUT)/modules
	cp $(OUT)/rootfs/images/rootfs.ext4 $(IMAGES)/
endif

# ---------- 合成 sdcard.img ----------
images:
ifeq ($(CONFIG_SDK_MAKE_SDIMAGE),y)
	$(OUT)/rootfs/host/bin/mkimage -A arm -O linux -T script -C none \
		-d $(BOARD)/boot.cmd $(IMAGES)/boot.scr
	rm -rf $(OUT)/genimage.tmp
	$(OUT)/rootfs/host/bin/genimage \
		--rootpath $(OUT)/rootfs/target \
		--tmproot $(OUT)/genimage.tmp \
		--inputpath $(IMAGES) \
		--outputpath $(IMAGES) \
		--config $(BOARD)/genimage.cfg
	@echo "==== 完成: $(IMAGES)/sdcard.img ===="
endif

clean:
	rm -rf $(OUT)
