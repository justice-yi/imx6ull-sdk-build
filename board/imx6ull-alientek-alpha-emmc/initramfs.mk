# initramfs.mk —— 板级 initramfs（本文件存在即启用；由 SDK Makefile 的 $(BOARD)/*.mk 通用加载）
# 分层：机制开关 CONFIG_BLK_DEV_INITRD=y 在 kernel defconfig 常开（空 initramfs 零副作用）；
# 本片段只管"内容"——打包 board/<板>/initramfs/ 并把动态绝对路径注入 CONFIG_INITRAMFS_SOURCE。
# 注入用内核自带 scripts/config（kbuild 不认命令行 CONFIG_ 变量，命令行传会触发环境重探）。

IRAMFS := $(IMAGES)/rootfs-initramfs.cpio.gz

.PHONY: initramfs do-initramfs

initramfs:
	cp -f $(OUT)/rootfs/target/bin/busybox $(BOARD)/initramfs/bin/busybox
	cd $(BOARD)/initramfs && find . | cpio -o -H newc 2>/dev/null | gzip > $(IRAMFS)

# 挂到 kernel 构建前：KERNEL_PRE_HOOKS 由各板级片段自行追加，主 Makefile 统一依序调用
KERNEL_PRE_HOOKS += do-initramfs

do-initramfs: initramfs
	$(SDK_ROOT)/kernel/scripts/config --file $(OUT)/kernel/.config \
		--set-str CONFIG_INITRAMFS_SOURCE $(IRAMFS)
	$(MAKE) -C $(SDK_ROOT)/kernel O=$(OUT)/kernel ARCH=arm CROSS_COMPILE=$(CROSS) olddefconfig
