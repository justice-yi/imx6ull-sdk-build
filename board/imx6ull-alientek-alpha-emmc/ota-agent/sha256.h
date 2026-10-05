/* sha256.h —— 自包含 SHA-256（公有域风格实现，ota-agent 用，不引第三方库） */
#ifndef OTA_SHA256_H
#define OTA_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t state[8];
	uint64_t bitlen;
	uint8_t  buf[64];
	size_t   buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);

/* 便捷：对整个文件（或块设备）计算，返回十六进制串（caller free）。
 * skip_bytes: 起始跳过（暂未用，留 0）。失败返回 NULL */
char *sha256_file_hex(const char *path, uint64_t max_bytes);

#endif
