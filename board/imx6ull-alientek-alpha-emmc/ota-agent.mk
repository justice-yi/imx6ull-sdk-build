# ota-agent.mk —— 板级烧录代理：存在即启用（SDK Makefile 经 $(BOARD)/*.mk 通用加载）
# 交叉编译 → 装进 rootfs-overlay/usr/sbin（该路径已 .gitignore，二进制不入库）
# 改完源码后: make ota-agent && make rootfs images
.PHONY: ota-agent

OTA_AGENT_SRC := $(BOARD)/ota-agent
OTA_AGENT_DST := $(BOARD)/rootfs-overlay/usr/sbin/ota-agent

ota-agent:
	mkdir -p $(dir $(OTA_AGENT_DST))
	$(CROSS)gcc -Wall -O2 -static -o $(OTA_AGENT_DST) \
		$(OTA_AGENT_SRC)/ota-agent.c $(OTA_AGENT_SRC)/sha256.c
	@echo "[ota-agent] 静态编译完成 → rootfs-overlay/usr/sbin（make rootfs images 带入镜像）"
