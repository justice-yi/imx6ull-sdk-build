# 用 ${mmcdev}/${mmcpart}——即 U-Boot 自己发现 boot.scr 的设备和分区，避免 SD/eMMC 编号差异
setenv bootargs console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw
fatload mmc ${mmcdev}:${mmcpart} ${loadaddr} zImage
fatload mmc ${mmcdev}:${mmcpart} ${fdt_addr} imx6ull-alientek-emmc.dtb
bootz ${loadaddr} - ${fdt_addr}
