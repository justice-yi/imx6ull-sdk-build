# 用 ${mmcdev}/${mmcpart}——即 U-Boot 自己发现 boot.scr 的设备和分区，避免 SD/eMMC 编号差异
# Linux 的 mmcblk 编号与 U-Boot 设备号恒等（2026-10-03 /proc/partitions 实测：
# TF=mmcblk0、eMMC=mmcblk1，与 NXP mx6ullevk.h 注释一致），root 用 mmcdev 直接拼出
setenv bootargs console=ttymxc0,115200 root=/dev/mmcblk${mmcdev}p2 rootwait rw
fatload mmc ${mmcdev}:${mmcpart} ${loadaddr} zImage
fatload mmc ${mmcdev}:${mmcpart} ${fdt_addr} imx6ull-alientek-emmc.dtb
bootz ${loadaddr} - ${fdt_addr}
