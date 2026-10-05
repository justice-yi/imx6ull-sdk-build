#!/bin/bash
# mk-image.sh —— 自研镜像组装器：layout.csv → board.img（genimage 退役）
# 用法: mk-image.sh <layout.csv> <images_dir>
# 依赖（WSL 宿主工具，README 环境准备节）: sfdisk mkfs.ext4 mkdosfs mcopy
# 单位约定（实测，各工具不同——全部在解析层换算成字节，脚本内部只用字节）:
#   sfdisk 的 start/size 用扇区(512B)；mkdosfs -C 的容量参数是 KiB；offset 必须 MiB 对齐
set -euo pipefail

CSV="$1"; IMGDIR="$2"; OUT="$IMGDIR/board.img"
SECTOR=512; MIB=1048576

die() { echo "mk-image: 错误: $*" >&2; exit 1; }
[ -f "$CSV" ] || die "找不到布局文件 $CSV"

# ---- ① 解析 CSV：跳过 # 注释行，M/G 后缀换算字节，MiB 对齐校验 ----
PARTS=$(awk -F',' '
	function tobytes(s) {
		if (s ~ /G$/) { sub(/G$/, "", s); return s * 1073741824 }
		if (s ~ /M$/) { sub(/M$/, "", s); return s * 1048576 }
		return s + 0
	}
	/^#/ { next } /^$/ { next }
	{
		# CSV 列允许对齐用前导/尾随空格，逐列 trim
		for (i = 1; i <= 7; i++) { gsub(/^ +| +$/, "", $i) }
		off = tobytes($2); size = tobytes($3)
		if (off % 1048576 != 0 || size % 1048576 != 0) {
			printf "偏移/尺寸必须 MiB 对齐: %s\n", $1 > "/dev/stderr"; exit 1
		}
		printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n", $1, off, size, $4, $5, $6, $7
	}' "$CSV") || die "CSV 解析失败"

# 收进数组 + 算总尺寸
NAMES=(); OFFS=(); SIZES=(); PTYPES=(); FSTYPES=(); LABELS=(); SOURCES=()
TOTAL=0
while IFS=$'\t' read -r n o s t f l src; do
	NAMES+=("$n"); OFFS+=("$o"); SIZES+=("$s"); PTYPES+=("$t")
	FSTYPES+=("$f"); LABELS+=("$l"); SOURCES+=("$src")
	end=$((o + s)); [ "$end" -gt "$TOTAL" ] && TOTAL=$end
done <<< "$PARTS"
[ ${#NAMES[@]} -gt 0 ] || die "CSV 无有效分区行"
echo "布局: ${#NAMES[@]} 个分区, 镜像总尺寸 $((TOTAL / MIB))MiB (sparse)"

# ---- ② 空镜像 + sfdisk 打 MBR ----
rm -f "$OUT"; truncate -s "$TOTAL" "$OUT"
SFDISK_IN="label: dos"
for i in "${!NAMES[@]}"; do
	line="start=$((OFFS[i] / SECTOR)), size=$((SIZES[i] / SECTOR)), type=${PTYPES[i]}"
	[ "${NAMES[i]}" = "boot" ] && line="$line, bootable"   # bootable 约定挂在名为 boot 的分区
	SFDISK_IN+=$'\n'"$line"
done
printf '%s\n' "$SFDISK_IN" | sfdisk --no-reread --no-tell-kernel -q "$OUT"

# ---- ③ u-boot 裸区：1KiB 偏移，i.MX ROM 硬约束，与分区表无关 ----
[ -f "$IMGDIR/u-boot-dtb.imx" ] || die "缺 $IMGDIR/u-boot-dtb.imx"
dd if="$IMGDIR/u-boot-dtb.imx" of="$OUT" bs=512 seek=2 conv=notrunc status=none

# ---- ④⑤ 逐分区写内容（source 三种语义；boot 分区内容物清单与 boot.cmd 对应）----
VFAT_FILES=(zImage imx6ull-alientek-emmc.dtb boot.scr)
for i in "${!NAMES[@]}"; do
	name=${NAMES[i]}; off=${OFFS[i]}; size=${SIZES[i]}
	src=${SOURCES[i]}; fst=${FSTYPES[i]}; label=${LABELS[i]}
	seek=$((off / MIB))
	case "$src" in
	-)	continue ;;   # 只建表：extended（空容器）
	empty)   # 空 ext4：临时生成（卷标取 label 列），写完即删
		tmp=$(mktemp)
		truncate -s "$size" "$tmp"
		mkfs.ext4 -q -F -L "$label" "$tmp"
		dd if="$tmp" of="$OUT" bs=1M seek=$seek conv=notrunc status=none
		rm -f "$tmp"
		echo "  $name: 空ext4 ${label:+(LABEL=$label)} @$((off / MIB))MiB"
		;;
	*)
		if [ "$fst" = "vfat" ]; then
			# boot 分区：现场打 FAT 镜像再写入（容量单位 KiB——实测；
			# mkdosfs -C 拒绝已存在文件，mktemp 出名后先删再建）
			tmp=$(mktemp); rm -f "$tmp"
			mkdosfs -n BOOT -C "$tmp" $((size / 1024)) >/dev/null
			for f in "${VFAT_FILES[@]}"; do
				[ -f "$IMGDIR/$f" ] || die "缺 $IMGDIR/$f（boot 分区内容物）"
				mcopy -i "$tmp" -s "$IMGDIR/$f" :: >/dev/null 2>&1
			done
			dd if="$tmp" of="$OUT" bs=1M seek=$seek conv=notrunc status=none
			rm -f "$tmp"
			echo "  $name: vfat(${VFAT_FILES[*]}) @$((off / MIB))MiB"
		else
			# 普通文件直写分区起始；分区剩余空间保持镜像里的零——无需预补尾
			[ -f "$IMGDIR/$src" ] || die "缺 $IMGDIR/$src（分区 $name）"
			dd if="$IMGDIR/$src" of="$OUT" bs=1M seek=$seek conv=notrunc status=none
			echo "  $name: $src @$((off / MIB))MiB"
		fi
		;;
	esac
done

# ---- ⑥ 逐分区 sha256（manifest 与 misc 出厂账本共用一份，避免二次读盘）----
DEVICE=$(basename "$(dirname "$(realpath "$CSV")")")
VERSION=$(date +%Y%m%d.%H%M)
HASHES=()
for i in "${!NAMES[@]}"; do
	if [ "${SOURCES[i]}" = "-" ]; then HASHES[i]="-"; continue; fi
	HASHES[i]=$(dd if="$OUT" bs=1M skip=$((OFFS[i] / MIB)) count=$((SIZES[i] / MIB)) \
		status=none | sha256sum | awk '{print $1}')
done

# ---- ⑥- uboot 裸区条目：③ 已写入镜像，此处算哈希供 ⑥a 账本与 ⑦ manifest 共用。
# 非分区（sfdisk 无此行）；ptype=raw 是 agent 的直写信号——commit 时按 offset
# 写整盘而非开 pN。1KiB 起（ROM 硬约束），必须整体装进 p1 前的裸区间隙。
UBOOT_SIZE=$(stat -c %s "$IMGDIR/u-boot-dtb.imx")
[ $((1024 + UBOOT_SIZE)) -le "${OFFS[0]}" ] \
	|| die "u-boot(${UBOOT_SIZE}B) 装不进裸区间隙(1KiB..p1@$((${OFFS[0]} / MIB))MiB)"
UBOOT_SHA=$(dd if="$OUT" iflag=skip_bytes,count_bytes bs=1M \
	skip=1024 count="$UBOOT_SIZE" status=none | sha256sum | awk '{print $1}')

# ---- ⑥a misc 出厂账本：预装 ota-state，首版整卡 dd 出来的板子体检即有记录 ----
# 放 misc（产线数据定位；烧/清 data 不毁台账）。misc 自身记 "-"：账本文件
# 影响所在分区 sha，自记自循环。种子只放 ota-state——provisioned 决不能预置
# （否则 firstboot 会被跳过）。
MISC_IDX=-1
for i in "${!NAMES[@]}"; do [ "${NAMES[i]}" = "misc" ] && MISC_IDX=$i; done
if [ "$MISC_IDX" -ge 0 ] && [ "${SOURCES[MISC_IDX]}" = "empty" ]; then
	SEED=$(mktemp -d)
	{
		echo "# version=$VERSION"
		for i in "${!NAMES[@]}"; do
			if [ "$i" -eq "$MISC_IDX" ]; then
				echo "${NAMES[i]} -"
			else
				echo "${NAMES[i]} ${HASHES[i]}"
			fi
		done
		echo "uboot ${UBOOT_SHA}"
	} > "$SEED/ota-state"
	tmp=$(mktemp)
	truncate -s "${SIZES[MISC_IDX]}" "$tmp"
	mkfs.ext4 -q -F -L "${LABELS[MISC_IDX]}" -d "$SEED" "$tmp"
	dd if="$tmp" of="$OUT" bs=1M seek=$((OFFS[MISC_IDX] / MIB)) conv=notrunc status=none
	rm -f "$tmp"; rm -rf "$SEED"
	# misc 终态重算（账本已在内），manifest 用终态值
	HASHES[MISC_IDX]=$(dd if="$OUT" bs=1M skip=$((OFFS[MISC_IDX] / MIB)) \
		count=$((SIZES[MISC_IDX] / MIB)) status=none | sha256sum | awk '{print $1}')
	echo "  misc: 空ext4(LABEL=${LABELS[MISC_IDX]}) + 出厂账本 ota-state @$((OFFS[MISC_IDX] / MIB))MiB"
fi

# ---- ⑥b 收尾校验：fdisk 读回对拍 ----
echo "== 分区表对拍（应与 layout.csv 一致） =="
sfdisk -d "$OUT"

# ---- ⑦ 生成 manifest 并尾部内嵌（烧录工具的镜像自描述）----
# 格式：[manifest CSV][4B len LE][4B CRC32(IEEE)][8B magic "IMX6OTA1"]，工具从文件尾倒读
# sha256 = 对镜像内该分区整个区间计算（板端整分区校验时的比对基准）；
# source=- 的分区（env，运行时状态）sha256 记 "-"，工具跳过校验
MAN=$(mktemp)
{
	echo "# version=$VERSION"
	echo "# device=$DEVICE"
	echo "# name, offset, size, ptype, fstype, sha256"
	for i in "${!NAMES[@]}"; do
		echo "${NAMES[i]}, ${OFFS[i]}, ${SIZES[i]}, ${PTYPES[i]}, ${FSTYPES[i]}, ${HASHES[i]}"
	done
	# uboot 裸区行：ptype=raw（分区行永远是 MBR 十六进制码，不混淆）——
	# agent 见 raw 按 offset 直写整盘，不参与行序→pN 映射，插哪都不影响分区寻址
	echo "uboot, 1024, ${UBOOT_SIZE}, raw, raw, ${UBOOT_SHA}"
} > "$MAN"

MLEN=$(stat -c %s "$MAN")
MCRC=$(python3 -c "import zlib,sys;print('%08x' % (zlib.crc32(open(sys.argv[1],'rb').read()) & 0xffffffff))" "$MAN")
cat "$MAN" >> "$OUT"
python3 - "$OUT" "$MLEN" "$MCRC" <<'PYEOF'
import struct, sys
out, mlen, mcrc = sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 16)
with open(out, 'ab') as f:
    f.write(struct.pack('<II', mlen, mcrc) + b'IMX6OTA1')
PYEOF
rm -f "$MAN"

# 回读自检：从尾倒解，magic/长度/CRC 必须还原
python3 - "$OUT" <<'PYEOF'
import struct, zlib, sys, os
with open(sys.argv[1], 'rb') as f:
    f.seek(0, os.SEEK_END); end = f.tell()
    f.seek(end - 16); desc = f.read(16)
    assert desc[8:] == b'IMX6OTA1', 'magic 校验失败'
    mlen, mcrc = struct.unpack('<II', desc[:8])
    f.seek(end - 16 - mlen); man = f.read(mlen)
    assert zlib.crc32(man) & 0xffffffff == mcrc, 'CRC 校验失败'
    print(f"manifest 内嵌自检通过: {mlen}B, crc={mcrc:08x}")
    for line in man.decode().splitlines():
        if not line.startswith('#'):
            print('  ' + line.split(',')[0].strip() + ' ✓')
PYEOF
echo "==== 完成: $OUT ($((TOTAL / MIB))MiB, 含尾部 manifest) ===="
