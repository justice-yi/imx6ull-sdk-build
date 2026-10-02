# 用 ${mmcdev}/${mmcpart}——即 U-Boot 自己发现 boot.scr 的设备和分区，避免 SD/eMMC 编号差异
# Linux 枚举此板固定 eMMC=mmcblk0、TF=mmcblk1（usdhc2 先注册），root 按启动介质分派
if test ${mmcdev} = 1; then setenv rootdev /dev/mmcblk0p2; else setenv rootdev /dev/mmcblk1p2; fi
setenv bootargs console=ttymxc0,115200 root=${rootdev} rootwait rw
fatload mmc ${mmcdev}:${mmcpart} ${loadaddr} zImage
fatload mmc ${mmcdev}:${mmcpart} ${fdt_addr} imx6ull-alientek-emmc.dtb
bootz ${loadaddr} - ${fdt_addr}
