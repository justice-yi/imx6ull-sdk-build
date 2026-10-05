# ============================================================
# IMX6ULL SDK 顶层编排 Makefile —— 在 build/ 下运行
#   make menuconfig   配置 SDK
#   make              全量构建（uboot→kernel→rootfs→board.img）
#   make uboot / kernel / rootfs / images   单独构建某环
#   make X-menuconfig / X-savedefconfig / X-defconfig
#                     组件配置流，X=uboot|kernel|rootfs（详见文件内注释节）
#   make clean        清空 output/
# ============================================================
SDK_ROOT  := $(abspath ..)
OUT       := $(SDK_ROOT)/output
IMAGES    := $(OUT)/images
KC        := scripts/kconfig

-include .config                       # 载入 menuconfig 结果
quote    = $(subst ",,$(1))
CROSS    := $(SDK_ROOT)/$(call quote,$(CONFIG_SDK_CROSS_PREFIX))
TC_ROOT  := $(shell realpath "$(dir $(CROSS))..")  # 工具链根目录（bin/.. 归一化）
JOBS     := $(CONFIG_SDK_JOBS)
DTB      := $(call quote,$(CONFIG_SDK_KERNEL_DTB))
# 板级目录：有 .config 用其值；defconfig 首跑（还没有 .config）时落到默认板
BOARD_NAME := $(call quote,$(CONFIG_SDK_BOARD_NAME))
ifeq ($(strip $(BOARD_NAME)),)
BOARD_NAME := imx6ull-alientek-alpha-emmc
endif
BOARD    := board/$(BOARD_NAME)
UBOOT_DEF   := mx6ull_alientek_emmc_defconfig
KERNEL_DEF  := imx6ull-alientek-emmc_defconfig

.PHONY: all menuconfig defconfig oldconfig savedefconfig \
        check uboot kernel rootfs images clean \
        uboot-menuconfig uboot-savedefconfig uboot-defconfig \
        kernel-menuconfig kernel-savedefconfig kernel-defconfig \
        rootfs-menuconfig rootfs-savedefconfig rootfs-defconfig

all: check uboot kernel rootfs images

check:
ifndef CONFIG_SDK_BOARD_NAME
	$(error 先运行 make defconfig 或 make menuconfig)
endif
	@test -x $(CROSS)gcc || { echo "工具链不存在: $(CROSS)gcc"; exit 1; }
	@mkdir -p $(OUT)/images $(OUT)/modules

# ---------- SDK 自身配置（kconfig） ----------
$(KC)/conf:
	$(MAKE) -C $(KC) conf

$(KC)/mconf:
	$(MAKE) -C $(KC) mconf
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
	@test -f $(OUT)/uboot/.config || $(MAKE) -C $(SDK_ROOT)/uboot O=$(OUT)/uboot CROSS_COMPILE=$(CROSS) $(UBOOT_DEF)
	$(MAKE) -C $(SDK_ROOT)/uboot O=$(OUT)/uboot CROSS_COMPILE=$(CROSS) -j$(JOBS)
	cp $(OUT)/uboot/u-boot-dtb.imx $(IMAGES)/
endif

# 板级构建片段：board/<板>/*.mk 自动加载（板级特性自治，本 Makefile 只提供
# KERNEL_PRE_HOOKS 通用钩子位，自身不感知任何具体特性）
include $(wildcard $(BOARD)/*.mk)

# ---------- 内核 ----------
kernel:
ifeq ($(CONFIG_SDK_BUILD_KERNEL),y)
	@test -f $(OUT)/kernel/.config || $(MAKE) -C $(SDK_ROOT)/kernel O=$(OUT)/kernel ARCH=arm CROSS_COMPILE=$(CROSS) $(KERNEL_DEF)
	$(if $(KERNEL_PRE_HOOKS),$(MAKE) $(KERNEL_PRE_HOOKS))
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
	@test -f $(OUT)/rootfs/.config || $(MAKE) -C $(SDK_ROOT)/buildroot O=$(OUT)/rootfs BR2_DEFCONFIG=$(OUT)/rootfs/defconfig defconfig
	$(MAKE) -C $(SDK_ROOT)/buildroot O=$(OUT)/rootfs SDK_MODULES_DIR=$(OUT)/modules
	cp $(OUT)/rootfs/images/rootfs.ext4 $(IMAGES)/
	cp -f $(OUT)/rootfs/images/rootfs.squashfs $(IMAGES)/
endif

# ---------- 组件配置流（X = uboot / kernel / rootfs）----------
#   X-menuconfig     直改 output/X/.config（构建目标已不重灌，改动直接生效）
#   X-savedefconfig  固化回源 defconfig（kernel/uboot 从 O= 目录通用名搬回；
#                    buildroot 写 BR2_DEFCONFIG 登记的生成文件，再反向还原
#                    @SDK_TOOLS@/@BOARD_DIR@ 占位符后落回 board 源文件）
#   X-defconfig      强制重灌（手改 defconfig 源文件后的显式生效动作）
uboot-menuconfig:
	$(MAKE) -C $(SDK_ROOT)/uboot O=$(OUT)/uboot CROSS_COMPILE=$(CROSS) menuconfig

uboot-savedefconfig:
	$(MAKE) -C $(SDK_ROOT)/uboot O=$(OUT)/uboot CROSS_COMPILE=$(CROSS) savedefconfig
	cp $(OUT)/uboot/defconfig $(SDK_ROOT)/uboot/configs/$(UBOOT_DEF)

uboot-defconfig:
	$(MAKE) -C $(SDK_ROOT)/uboot O=$(OUT)/uboot CROSS_COMPILE=$(CROSS) $(UBOOT_DEF)

kernel-menuconfig:
	$(MAKE) -C $(SDK_ROOT)/kernel O=$(OUT)/kernel ARCH=arm CROSS_COMPILE=$(CROSS) menuconfig

# 注：CONFIG_INITRAMFS_SOURCE 由板级片段（initramfs.mk）构建时动态注入，
# 固化前剔除，绝对路径永不落入源 defconfig
kernel-savedefconfig:
	$(MAKE) -C $(SDK_ROOT)/kernel O=$(OUT)/kernel ARCH=arm CROSS_COMPILE=$(CROSS) savedefconfig
	sed -i '/^CONFIG_INITRAMFS_SOURCE=/d' $(OUT)/kernel/defconfig
	cp $(OUT)/kernel/defconfig $(SDK_ROOT)/kernel/arch/arm/configs/$(KERNEL_DEF)

kernel-defconfig:
	$(MAKE) -C $(SDK_ROOT)/kernel O=$(OUT)/kernel ARCH=arm CROSS_COMPILE=$(CROSS) $(KERNEL_DEF)

rootfs-menuconfig:
	$(MAKE) -C $(SDK_ROOT)/buildroot O=$(OUT)/rootfs menuconfig

rootfs-savedefconfig:
	$(MAKE) -C $(SDK_ROOT)/buildroot O=$(OUT)/rootfs savedefconfig
	sed -e 's|$(TC_ROOT)|@SDK_TOOLS@|g' -e 's|$(realpath $(BOARD))|@BOARD_DIR@|g' \
		$(OUT)/rootfs/defconfig > $(BOARD)/buildroot_defconfig

rootfs-defconfig: $(OUT)/rootfs/defconfig
	$(MAKE) -C $(SDK_ROOT)/buildroot O=$(OUT)/rootfs BR2_DEFCONFIG=$(OUT)/rootfs/defconfig defconfig

# ---------- 合成 board.img（自研 mk-image.sh + layout.csv，genimage 退役）----------
images:
ifeq ($(CONFIG_SDK_MAKE_SDIMAGE),y)
	$(OUT)/rootfs/host/bin/mkimage -A arm -O linux -T script -C none \
		-d $(BOARD)/boot.cmd $(IMAGES)/boot.scr
	scripts/mk-image.sh $(BOARD)/layout.csv $(IMAGES)
endif

clean:
	rm -rf $(OUT)
