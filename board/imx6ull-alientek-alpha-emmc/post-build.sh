#!/bin/sh
# buildroot 在生成文件系统镜像前调用，$1 = target 目录
# SDK 通过环境变量 SDK_MODULES_DIR 告知模块位置（Makefile 里传入）
set -e
TARGET_DIR="$1"

if [ -n "$SDK_MODULES_DIR" ] && [ -d "$SDK_MODULES_DIR/lib/modules" ]; then
	mkdir -p "$TARGET_DIR/lib"
	cp -a "$SDK_MODULES_DIR/lib/modules" "$TARGET_DIR/lib/"
	echo "[post-build] 内核模块已装入 rootfs"
fi
