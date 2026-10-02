setenv bootargs console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw
fatload mmc 1:1 ${loadaddr} zImage
fatload mmc 1:1 ${fdt_addr} imx6ull-alientek-emmc.dtb
bootz ${loadaddr} - ${fdt_addr}
