# 出厂自愈（2026-10-04）：整卡 dd 后 env 分区全零；fw_setenv 若在坏 env 上续写
# 会写入其内置远古默认（NFS 启动）导致下次无法自启。首启检测 env 首扇区全零即
# saveenv，把 U-Boot 编译默认（含 board_late_mmc_env_init 按启动介质修正的
# mmcdev）固化进 env 分区——fw 工具从此永远在合法底稿上续写。
# 0x20800 = CONFIG_ENV_OFFSET 0x4100000 / 512；开销=单扇区读，毫秒级。
# 注意：本块必须位于 setenv bootargs 之前，保证存入的是干净默认环境。
mw.b ${loadaddr} 0 0x200
mmc read ${fdt_addr} 0x20800 1
if cmp.b ${loadaddr} ${fdt_addr} 0x200; then saveenv; fi

# 用 ${mmcdev}/${mmcpart}——即 U-Boot 自己发现 boot.scr 的设备和分区，避免 SD/eMMC 编号差异
# Linux 的 mmcblk 编号与 U-Boot 设备号恒等（2026-10-03 /proc/partitions 实测：
# TF=mmcblk0、eMMC=mmcblk1，与 NXP mx6ullevk.h 注释一致），root 用 mmcdev 直接拼出
# slot 由 U-Boot env 决定（A 缺省）；MBR 超 4 分区走扩展分区，A 槽=p5、B 槽=p6（逻辑分区）
if test "${slot}" = "B"; then setenv rootpart 6; else setenv rootpart 5; fi
setenv bootargs console=ttymxc0,115200 root=/dev/mmcblk${mmcdev}p${rootpart} rootwait rw
fatload mmc ${mmcdev}:1 ${loadaddr} zImage
fatload mmc ${mmcdev}:1 ${fdt_addr} imx6ull-alientek-emmc.dtb
bootz ${loadaddr} - ${fdt_addr}
