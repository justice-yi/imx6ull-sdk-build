# IMX6ULL SDK

repo 管理的 i.MX6ULL（正点原子 Alpha 板）嵌入式开发环境。

## 组件

| 组件 | 版本 | 仓库（GitHub justice-yi） |
|---|---|---|
| U-Boot | 2022.04 | uboot-imx |
| Linux | 5.15.31 | linux-imx-git.kernel.org-linux-stable-linux-5.15.y |
| Buildroot | 2022.11.1 | buildroot22.2 |
| 工具链 | Bootlin GCC 11.3（glibc, 自带于 tools/） | tools |
| 构建系统 | 本仓库 | imx6ull-sdk-build |

## 目录结构

```
imx6ull-sdk/
├── kernel/  uboot/  buildroot/  tools/   组件仓库（repo 管理）
├── build/   Makefile / Kconfig / 板级配置 / 本 README
└── output/  构建产物（images/sdcard.img）
```

## 环境准备（新机器，一次性）

```bash
sudo apt install libncurses-dev libssl-dev libelf-dev libgmp-dev libmpc-dev libmpfr-dev \
                 lzop cpio unzip mtools dosfstools repo
```

## 拉起 SDK

```bash
repo init --no-repo-verify \
  --repo-url https://github.com/justice-yi/git-repo \
  -u https://github.com/justice-yi/imx6ull-sdk-manifest
repo sync
```

## 构建与烧写

```bash
cd build
make defconfig      # 载入板级默认配置
make menuconfig     # 可选：调整配置
make                # 全量构建
```

产物 `output/images/sdcard.img`（U-Boot 裸放 1KiB + vfat(zImage/dtb/boot.scr) + ext4 rootfs）：

```bash
sudo dd if=output/images/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
```

上板：TF 卡启动档位，串口 115200（UART1）。U-Boot 自动执行 boot.scr，免手动环境变量。

同一镜像可烧 eMMC 启动：在 TF 起的系统里 `dd if=sdcard.img of=/dev/mmcblk1 bs=4M conv=fsync`
（实测 mmcblk 与 U-Boot 设备号恒等：TF=mmcblk0、eMMC=mmcblk1；boot.scr 按 mmcdev 拼 root，双介质通用），然后拨码切 eMMC 档。

日常增量：内核/U-Boot 改动可只 `dd` 对应产物（uboot 段 `bs=512 seek=2`），或挂载卡分区替换 zImage/dtb。

## 日常开发

```bash
# 改代码前先挂分支（repo sync 后各仓是 detached HEAD，直接提交会悬空）
cd kernel && git checkout -B master
# ...改码、编译验证（cd ../build && make kernel）
git add -A && git commit -m "..." && git push origin master
```

更新整个 SDK：`repo sync`。

## 已知事项

- **以太网不可用**：设备树 PHY 配置与该板不符，待改 `kernel/arch/arm/boot/dts/imx6ull-alientek-emmc.dts` 的 FEC 节点；
- **SDMA 固件警告**：如需消除，buildroot 开 `BR2_PACKAGE_LINUX_FIRMWARE`（含 imx sdma）后重出 rootfs；
- U-Boot 环境变量首次上电有 bad CRC 警告，板上 `saveenv` 一次即消。

## 网络与认证（WSL 环境）

- GitHub 访问经 Windows 云链代理（Allow LAN，混合端口 12450）：
  gitconfig 已按 `github.com` 域配置代理；repo 启动器用 `~/.local/bin/repo`
  （自带 shim，自动以当前网关 IP 注入代理，仅进程内生效）；
- 私有仓认证：`~/.git-credentials`（credential helper store）。token 到期时替换该文件中的 token 即可；
- 代理端口变更：改 gitconfig 的 `http.https://github.com.proxy` 和启动器 shim 中两处 `12450`；
- WSL 网关 IP 重启后可能变化，shim 自动适应，gitconfig 不适应（需手动改）。
