/*
 * ota-agent.c —— 板端烧录代理 daemon（服务模式，纯 C 无第三方库）
 *
 * 用法:
 *   ota-agent daemon [port]   常驻服务（默认 :8080），init 脚本开机自启；
 *                             自带后台化（OTA_AGENT_FG=1 前台跑，调试用）
 *   ota-agent query [disk]    调试：打印板端分区状态 CSV（全盘 sha256，慢）
 *   ota-agent setslot A|B     调试：手动翻槽
 *
 * 服务模式（PC flashtool 为 HTTP 客户端，全部交互 PC 发起）：
 *   GET  /status         轻量体检：安装记录+断点+槽+上次烧录版本（不 hash 分区，毫秒级）
 *   GET  /query          深度体检：全分区实时 sha256（慢，调试用）
 *   POST /manifest       镜像 manifest；内容变了自动作废 /data/.ota 旧断点
 *   POST /select         当前勾选清单（全量覆盖；/part 只收清单内的分区）
 *   POST /part/<名>      分块上传数据，头 X-Offset: N 续传写 /data/.ota/<名>.part
 *   POST /verify/<名>    板端对临时文件算 sha256 与 manifest 对账（GET 同效；
 *                        流式响应：每 16MiB 回一行 part=.. stage=verify got=..，
 *                        尾行 ok / fail <实算sha>——长活儿的真实进度板端实时报）
 *   POST /commit/<名>    dd 进 /dev/mmcblk0pN + fsync + 写安装记录 + 删临时文件
 *                        （GET 同效；流式响应同上，stage=burn）
 *   POST /slot/A|B       fw_setenv 翻槽
 *   POST /abort          删除全部断点临时文件
 *
 * 烧写目标盘写死 mmcblk0（DISK_DEF）。寻址规则：普通分区行按行序开 pN；
 * ptype=raw 行（如 uboot@1KiB 裸区）按 manifest 第2列 offset 直写整盘——
 * 裸区条目没有内核分区，且必须挡在最低分区起点之前，防错行烧穿分区表。
 *
 * 安全规则：
 *   - 数据永远先落 /data/.ota 临时文件，verify 通过才 dd，绝不边收边烧
 *   - 扩展容器(ptype 0x0f)与 sha256=-(env 等)的分区拒收拒烧
 *   - /part 只接受 select 白名单内的分区名
 *   - 暂存区所在分区（data）拒收拒烧：dd 会覆盖在途临时文件，半成品直进
 *     分区——HTTP/WiFi 承载专属（暂存依赖文件系统）；将来裸 USB 入口流式
 *     直写，不经这两个 handler，不受此限
 *   - 安装记录 /misc/ota-state：<名> <sha256>（放 misc：烧/清 data 不毁台账，
 *     misc 定位即产线数据）。镜像出厂时由 mk-image.sh 预置（misc 自身不自记：
 *     账本文件影响所在分区 sha，自记自循环）
 */
#define _GNU_SOURCE	/* memmem/strcasestr */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <dirent.h>
#include "sha256.h"

#define OTA_DIR    "/data/.ota"
#define STATE_FILE "/misc/ota-state"
#define PID_FILE   "/run/ota-agent.pid"
#define DEF_PORT   8080
#define DISK_DEF   "mmcblk0"
#define HDR_MAX    8192
#define BODY_MAX   (64 * 1024)		/* manifest/select 等小 body 上限 */

/* ---------- 小工具 ---------- */
static void die(const char *msg)
{
	fprintf(stderr, "ota-agent: %s (%s)\n", msg, strerror(errno));
	exit(1);
}

static void nop(int sig) { (void)sig; }	/* SIGPIPE 免疫 */

static void trim(char *s)
{
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
	char *p = s;
	while (*p == ' ' || *p == '\t') p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
}

/* ---------- manifest（分区行按行序开 pN；ptype=raw 行按 offset 直写整盘） ---------- */
#define MAX_PARTS 16
struct part {
	char name[32];
	long long offset;		/* 第2列：raw 行=整盘写入偏移；分区行不用于寻址 */
	long long size;
	char ptype[8], fstype[16];
	char sha256[80];
};
static struct part P[MAX_PARTS];
static int NP;
static char g_man_ver[64];		/* manifest 头 # version= */
static char g_man_dev[64];		/* manifest 头 # device= */
static char g_man_buf[BODY_MAX];	/* 最近一次收到的原文（换镜像检测用） */

static int manifest_parse(const char *csv)
{
	char line[512];
	const char *p = csv;
	NP = 0;
	g_man_ver[0] = g_man_dev[0] = 0;
	while (*p && NP < MAX_PARTS) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (len >= sizeof(line)) len = sizeof(line) - 1;
		memcpy(line, p, len); line[len] = 0;
		p = nl ? nl + 1 : p + strlen(p);
		if (line[0] == '#' || line[0] == 0) {
			if (strncmp(line, "# version=", 10) == 0)
				snprintf(g_man_ver, sizeof(g_man_ver), "%s", line + 10);
			else if (strncmp(line, "# device=", 9) == 0)
				snprintf(g_man_dev, sizeof(g_man_dev), "%s", line + 9);
			continue;
		}
		struct part t = {0};
		char *c1 = strchr(line, ','); if (!c1) continue; *c1 = 0;
		char *c2 = strchr(c1+1, ','); if (!c2) continue; *c2 = 0;
		char *c3 = strchr(c2+1, ','); if (!c3) continue; *c3 = 0;
		char *c4 = strchr(c3+1, ','); if (!c4) continue; *c4 = 0;
		char *c5 = strchr(c4+1, ','); if (!c5) continue; *c5 = 0;
		snprintf(t.name, sizeof(t.name), "%s", line); trim(t.name);
		t.offset = atoll(c1+1);
		t.size = atoll(c2+1);
		snprintf(t.ptype, sizeof(t.ptype), "%s", c3+1); trim(t.ptype);
		snprintf(t.fstype, sizeof(t.fstype), "%s", c4+1); trim(t.fstype);
		snprintf(t.sha256, sizeof(t.sha256), "%s", c5+1); trim(t.sha256);
		if (t.name[0] && t.size > 0) P[NP++] = t;
	}
	return NP;
}

static struct part *part_find(const char *name)
{
	for (int i = 0; i < NP; i++)
		if (strcmp(P[i].name, name) == 0) return &P[i];
	return NULL;
}

/* ---------- 勾选白名单（PC 全量覆盖式推送） ---------- */
static char g_sel[MAX_PARTS][32];
static int g_nsel;

static int select_has(const char *name)
{
	for (int i = 0; i < g_nsel; i++)
		if (strcmp(g_sel[i], name) == 0) return 1;
	return 0;
}

/* ---------- 安装记录（/status 对账用，免去全盘 hash） ---------- */
struct rec { char name[32]; char sha[80]; };
static struct rec g_last[MAX_PARTS * 2];
static int g_nlast;
static char g_state_ver[64];		/* 上次烧录的 manifest 版本 */

static void state_set(const char *name, const char *sha)
{
	for (int i = 0; i < g_nlast; i++)
		if (strcmp(g_last[i].name, name) == 0) {
			snprintf(g_last[i].sha, sizeof(g_last[i].sha), "%s", sha);
			return;
		}
	if (g_nlast < (int)(sizeof(g_last) / sizeof(g_last[0]))) {
		snprintf(g_last[g_nlast].name, sizeof(g_last[0].name), "%s", name);
		snprintf(g_last[g_nlast].sha, sizeof(g_last[0].sha), "%s", sha);
		g_nlast++;
	}
}

static int state_get(const char *name, char *out, size_t cap)
{
	for (int i = 0; i < g_nlast; i++)
		if (strcmp(g_last[i].name, name) == 0) {
			snprintf(out, cap, "%s", g_last[i].sha);
			return 1;
		}
	return 0;
}

static void state_load(void)
{
	FILE *f = fopen(STATE_FILE, "r");
	if (!f) return;
	char line[256];
	g_nlast = 0;
	g_state_ver[0] = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "# version=", 10) == 0) {
			snprintf(g_state_ver, sizeof(g_state_ver), "%s", line + 10);
			trim(g_state_ver);
			continue;
		}
		char *sp = strchr(line, ' ');
		if (!sp) continue;
		*sp = 0;
		trim(line);
		trim(sp + 1);
		state_set(line, sp + 1);
	}
	fclose(f);
}

static void state_save(void)
{
	FILE *f = fopen(STATE_FILE ".tmp", "w");
	if (!f) return;
	fprintf(f, "# version=%s\n", g_state_ver);
	for (int i = 0; i < g_nlast; i++)
		fprintf(f, "%s %s\n", g_last[i].name, g_last[i].sha);
	fclose(f);
	rename(STATE_FILE ".tmp", STATE_FILE);
}

/* ---------- 槽位 ---------- */
static void slot_current(char *out, size_t cap)
{
	out[0] = 0;
	FILE *f = popen("fw_printenv slot 2>/dev/null", "r");
	if (!f) return;
	char buf[64] = {0};
	if (fgets(buf, sizeof(buf), f)) {
		char *eq = strchr(buf, '=');
		if (eq) {
			trim(eq + 1);
			snprintf(out, cap, "%s", eq + 1);
		}
	}
	pclose(f);
}

static int setslot(const char *ab)
{
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "fw_setenv slot %s", ab);
	return system(cmd);
}

/* ---------- 断点临时文件 ---------- */
static void tmp_path(const char *name, char *out, size_t cap)
{
	snprintf(out, cap, OTA_DIR "/%s.part", name);
}

static long long tmp_size(const char *name)
{
	char p[128];
	struct stat st;
	tmp_path(name, p, sizeof(p));
	if (stat(p, &st) == 0) return (long long)st.st_size;
	return 0;
}

static void clear_tmp(void)
{
	DIR *d = opendir(OTA_DIR);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n > 5 && strcmp(e->d_name + n - 5, ".part") == 0) {
			char p[160];
			snprintf(p, sizeof(p), OTA_DIR "/%s", e->d_name);
			unlink(p);
		}
	}
	closedir(d);
}

/* 60 秒内有没有 .part 被写过（=疑似在途上传） */
static int any_hot_part(void)
{
	DIR *d = opendir(OTA_DIR);
	if (!d) return 0;
	struct dirent *e;
	time_t now = time(NULL);
	int hot = 0;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n > 5 && strcmp(e->d_name + n - 5, ".part") == 0) {
			char p[160];
			struct stat st;
			snprintf(p, sizeof(p), OTA_DIR "/%s", e->d_name);
			if (stat(p, &st) == 0
			    && now - st.st_mtime < 60) {
				hot = 1;
				break;
			}
		}
	}
	closedir(d);
	return hot;
}

/* 暂存区所在分区判定：/proc/mounts 里找 OTA_DIR 的最长前缀挂载点，
 * 其设备号（/dev/mmcblk0pN，行序即分区号）对应到 manifest 行。
 * 检测不出（如 /data 未挂）返回 0——此时临时文件也写不了，天然安全 */
static int is_staging_part(const struct part *p)
{
	FILE *f = fopen("/proc/mounts", "r");
	if (!f) return 0;
	char line[512], dev[128], mp[256], bestmp[256] = "", bestdev[128] = "";
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%127s %255s", dev, mp) != 2) continue;
		size_t ml = strlen(mp);
		if (strncmp(OTA_DIR, mp, ml) == 0 && ml > strlen(bestmp)) {
			snprintf(bestmp, sizeof(bestmp), "%s", mp);
			snprintf(bestdev, sizeof(bestdev), "%s", dev);
		}
	}
	fclose(f);
	if (!bestmp[0]) return 0;
	char *pn = strrchr(bestdev, 'p');
	int n = pn ? atoi(pn + 1) : 0;
	return n >= 1 && n <= NP && &P[n - 1] == p;
}

/* ---------- 深度体检（全盘 sha256，调试用） ---------- */
static int query_print(const char *disk)
{
	char path[128], sysp[160], hex[80];
	printf("# disk=%s\n# pN, size_bytes, sha256\n", disk);
	for (int i = 1; i <= 32; i++) {
		snprintf(path, sizeof(path), "/dev/%sp%d", disk, i);
		snprintf(sysp, sizeof(sysp), "/sys/block/%s/%sp%d/size", disk, disk, i);
		FILE *f = fopen(sysp, "r");
		if (!f) continue;
		long long sec = 0;
		if (fscanf(f, "%lld", &sec) != 1) sec = 0;
		fclose(f);
		char *h = sha256_file_hex(path, sec * 512);
		snprintf(hex, sizeof(hex), "%s", h ? h : "-");
		free(h);
		printf("p%d, %lld, %s\n", i, sec * 512, hex);
		fflush(stdout);
	}
	return 0;
}

/* ---------- 极简 HTTP 服务端 ---------- */
static void respond(int c, int code, const char *ctype,
		    const char *body, long len)
{
	const char *rs = code == 200 ? "OK"
		       : code == 400 ? "Bad Request"
		       : code == 403 ? "Forbidden"
		       : code == 404 ? "Not Found"
		       : code == 413 ? "Too Large"
		       : "Server Error";
	char head[256];
	int n = snprintf(head, sizeof(head),
		"HTTP/1.0 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %ld\r\n"
		"Connection: close\r\n\r\n", code, rs, ctype, len);
	send(c, head, (size_t)n, 0);
	if (body && len > 0)
		send(c, body, (size_t)len, 0);
}

static void respond_text(int c, int code, const char *text)
{
	respond(c, code, "text/plain", text, (long)strlen(text));
}

/* 流式响应（verify/commit 用）：头不带 Content-Length（收发双方按
 * Connection: close 与"边算边 send"配合），进度行实时推给 PC——
 * 长静默操作的真实进度只有板端知道，PC 不猜 */
static void respond_stream_begin(int c)
{
	const char *h = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
			"Connection: close\r\n\r\n";
	send(c, h, (int)strlen(h), 0);
}

static void send_line(int c, const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		send(c, buf, n, 0);
}

struct req {
	char method[8];
	char path[128];
	long content_length;		/* -1=未知 */
	long long x_offset;		/* X-Offset，-1=无 */
};

/* 读到 \r\n\r\n；返回头区字节数（含空行），hdrend..total 是已带上的 body 首段 */
static int read_head(int c, char *buf, int bufcap, int *total, struct req *r)
{
	for (;;) {
		char *end = memmem(buf, (size_t)*total, "\r\n\r\n", 4);
		if (end) {
			int hdrend = (int)(end - buf) + 4;
			*end = 0;			/* 头区按串处理 */
			memset(r, 0, sizeof(*r));
			r->content_length = -1;
			r->x_offset = -1;
			if (sscanf(buf, "%7s %127s", r->method, r->path) != 2)
				return -1;
			char *q = strcasestr(buf, "content-length:");
			if (q) r->content_length = atol(q + 15);
			q = strcasestr(buf, "x-offset:");
			if (q) r->x_offset = atoll(q + 9);
			return hdrend;
		}
		if (*total >= bufcap)
			return -1;	/* 头区超限（大 body 与头同包到达时首包可能占满缓冲） */
		int n = recv(c, buf + *total, (size_t)(bufcap - *total), 0);
		if (n <= 0) return -1;
		*total += n;
	}
}

/* 收 Content-Length 字节 body：fd>=0 时流式写自 off 起；否则写进 mem_out */
static long long recv_body(int c, const char *buf, int hdrend, int total,
			   long cl, int fd, long long off, char *mem_out)
{
	long long done = 0;
	long first = total - hdrend;
	if (first > cl) first = cl;
	/* 定位必须无条件做：lseek 原先在 first>0 分支里，头包不带 body 时
	 * （TCP 分包运气）每块都从 0 写起——块间互相覆盖+零洞，大小不变
	 * 内容错乱，且单块(offset=0)测试永远测不出来 */
	if (fd >= 0 && lseek(fd, off, SEEK_SET) < 0)
		return -1;
	if (first > 0) {
		if (fd >= 0) {
			if (write(fd, buf + hdrend, (size_t)first) != first)
				return -1;
		} else {
			memcpy(mem_out, buf + hdrend, (size_t)first);
		}
		done = first;
	}
	char tmp[65536];
	while (done < cl) {
		long want = cl - (long)done;
		if (want > (long)sizeof(tmp)) want = (long)sizeof(tmp);
		int n = recv(c, tmp, (size_t)want, 0);
		if (n <= 0) return -1;
		if (fd >= 0) {
			if (write(fd, tmp, (size_t)n) != n) return -1;
		} else {
			memcpy(mem_out + done, tmp, (size_t)n);
		}
		done += n;
	}
	return done;
}

/* 收小 body 进堆（NUL 结尾）。NULL=失败（含超上限） */
static char *recv_body_mem(int c, char *buf, int hdrend, int total,
			   long cl, long cap)
{
	if (cl < 0 || cl > cap) return NULL;
	char *mem = malloc((size_t)cl + 1);
	if (!mem) return NULL;
	if (recv_body(c, buf, hdrend, total, cl, -1, 0, mem) != cl) {
		free(mem);
		return NULL;
	}
	mem[cl] = 0;
	return mem;
}

/* ---------- 各接口 ---------- */
static void handle_status(int c)
{
	char slot[16];
	slot_current(slot, sizeof(slot));
	char *b = malloc(8192);
	if (!b) { respond_text(c, 500, "oom"); return; }
	int n = snprintf(b, 8192, "# version=%s\n# slot=%s\n"
			 "# name, last_sha256, tmp_bytes\n",
			 g_state_ver[0] ? g_state_ver : "-", slot[0] ? slot : "-");
	for (int i = 0; i < NP && n < 8100; i++) {
		char sha[80];
		if (!state_get(P[i].name, sha, sizeof(sha)))
			snprintf(sha, sizeof(sha), "-");
		n += snprintf(b + n, 8192 - n, "%s, %s, %lld\n",
			      P[i].name, sha, tmp_size(P[i].name));
	}
	/* manifest 未推送时也把历史安装记录报出去（PC 按名合并） */
	for (int i = 0; i < g_nlast && n < 8100; i++) {
		if (part_find(g_last[i].name)) continue;
		n += snprintf(b + n, 8192 - n, "%s, %s, %lld\n",
			      g_last[i].name, g_last[i].sha,
			      tmp_size(g_last[i].name));
	}
	respond(c, 200, "text/csv", b, (long)strlen(b));
	free(b);
}

static void handle_query(int c)
{
	const char *hdr =
		"HTTP/1.0 200 OK\r\nContent-Type: text/csv\r\n"
		"Connection: close\r\n\r\n";
	send(c, hdr, (int)strlen(hdr), 0);
	int save = dup(1);
	dup2(c, 1);
	query_print(DISK_DEF);
	fflush(stdout);
	dup2(save, 1);
	close(save);
}

static void handle_manifest(int c, char *body, long len)
{
	(void)len;
	int changing = strcmp(g_man_buf, body) != 0;	/* 换镜像？ */
	if (changing && any_hot_part()) {
		/* 在途上传时拒收换 manifest——清掉正在写的 .part 会把
		 * 断点头删成稀疏零洞，产出"头零尾真"的混合文件
		 * （大小不变、哈希谁都不像，校验必败）。
		 * 拒收=解析都不做、状态原封，当前上传的 manifest 保持权威 */
		fprintf(stderr, "[ota-agent] 拒收换 manifest（在途上传，"
			"60s 内有 .part 在写）\n");
		respond_text(c, 409, "upload in progress");
		return;
	}
	if (manifest_parse(body) <= 0) {
		respond_text(c, 400, "manifest parse fail");
		return;
	}
	if (changing) {				/* 旧断点作废 */
		snprintf(g_man_buf, sizeof(g_man_buf), "%s", body);
		clear_tmp();
	}
	respond_text(c, 200, "ok");
}

static void handle_select(int c, char *body)
{
	g_nsel = 0;
	char *line = strtok(body, "\n");
	while (line && g_nsel < MAX_PARTS) {
		trim(line);
		if (line[0] && line[0] != '#' && strncmp(line, "slot=", 5) != 0)
			snprintf(g_sel[g_nsel++], sizeof(g_sel[0]), "%s", line);
		line = strtok(NULL, "\n");
	}
	respond_text(c, 200, "ok");
}

static void handle_part(int c, struct req *r, const char *name,
			char *buf, int hdrend, int total)
{
	struct part *p = part_find(name);
	if (!p) { respond_text(c, 404, "not in manifest"); return; }
	if (!select_has(name)) { respond_text(c, 403, "not selected"); return; }
	if (strncmp(p->ptype, "0x0f", 4) == 0 || p->ptype[0] == 'f') {
		respond_text(c, 403, "extended container"); return;
	}
	if (strcmp(p->sha256, "-") == 0) {
		respond_text(c, 403, "no sha baseline"); return;
	}
	if (is_staging_part(p)) {
		respond_text(c, 403, "staging partition (wifi mode)"); return;
	}
	if (r->content_length < 0 || r->x_offset < 0 || r->x_offset > p->size) {
		respond_text(c, 400, "bad offset/length"); return;
	}
	mkdir(OTA_DIR, 0755);
	char tmp[128];
	tmp_path(name, tmp, sizeof(tmp));
	int fd = open(tmp, O_WRONLY | O_CREAT, 0644);
	if (fd < 0) { respond_text(c, 500, "open tmp fail"); return; }
	long long n = recv_body(c, buf, hdrend, total,
				r->content_length, fd, r->x_offset, NULL);
	close(fd);
	if (n != r->content_length) {
		respond_text(c, 500, "recv body fail");
		return;
	}
	char rs[64];
	snprintf(rs, sizeof(rs), "ok %lld", tmp_size(name));
	respond(c, 200, "text/plain", rs, (long)strlen(rs));
}

static void handle_verify(int c, const char *name)
{
	struct part *p = part_find(name);
	if (!p) { respond_text(c, 404, "not in manifest"); return; }
	char tmp[128];
	tmp_path(name, tmp, sizeof(tmp));
	struct stat st;
	if (stat(tmp, &st) != 0 || (long long)st.st_size != p->size) {
		char rs[96];
		snprintf(rs, sizeof(rs), "fail size %lld/%lld",
			 stat(tmp, &st) == 0 ? (long long)st.st_size : -1LL,
			 p->size);
		respond(c, 200, "text/plain", rs, (long)strlen(rs));
		return;
	}
	fprintf(stderr, "[ota-agent] verify %s：sha256 %lldMiB 计算中…\n",
		name, p->size >> 20);
	respond_stream_begin(c);
	FILE *f = fopen(tmp, "rb");
	if (!f) {
		send_line(c, "fail open tmp\n");
		return;
	}
	sha256_ctx sc;
	uint8_t buf[65536], dg[32];
	uint64_t got = 0, last_rep = 0;
	size_t n;
	sha256_init(&sc);
	while (got < (uint64_t)p->size &&
	       (n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (got + n > (uint64_t)p->size)
			n = (size_t)((uint64_t)p->size - got);
		sha256_update(&sc, buf, n);
		got += n;
		if (got - last_rep >= 16 * 1024 * 1024
		    || got == (uint64_t)p->size) {		/* 每 16MiB 或收尾一报 */
			last_rep = got;
			send_line(c, "part=%s stage=verify got=%llu total=%llu\n",
				  name, (unsigned long long)got,
				  (unsigned long long)p->size);
		}
	}
	fclose(f);
	sha256_final(&sc, dg);
	char hex[65];
	for (int i = 0; i < 32; i++)
		sprintf(hex + i * 2, "%02x", dg[i]);
	hex[64] = 0;
	if (strcasecmp(hex, p->sha256) == 0)
		send_line(c, "ok\n");
	else
		send_line(c, "fail %s\n", hex);
	fprintf(stderr, "[ota-agent] verify %s：%s\n", name,
		strcasecmp(hex, p->sha256) == 0 ? "ok" : "fail");
}

static void handle_commit(int c, const char *name)
{
	struct part *p = part_find(name);
	if (!p) { respond_text(c, 404, "not in manifest"); return; }
	if (!select_has(name)) { respond_text(c, 403, "not selected"); return; }
	if (is_staging_part(p)) {
		respond_text(c, 403, "staging partition (wifi mode)"); return;
	}
	char tmp[128], dev[64];
	tmp_path(name, tmp, sizeof(tmp));
	/* ptype=raw：非分区裸区条目（如 uboot@1KiB），按 manifest offset 直写
	 * 整盘；分区行照旧行序开 pN。raw 越界防护：必须整体落在最低分区
	 * 起点之前——错行 offset 会烧穿分区表/分区，这里拒烧 */
	int raw_area = (strncmp(p->ptype, "raw", 3) == 0);
	if (raw_area) {
		long long min_part = -1;
		for (int i = 0; i < NP; i++)
			if (strncmp(P[i].ptype, "raw", 3) != 0 && P[i].offset > 0 &&
			    (min_part < 0 || P[i].offset < min_part))
				min_part = P[i].offset;
		if (p->offset <= 0 ||
		    (min_part > 0 && p->offset + p->size > min_part)) {
			respond_text(c, 400, "raw part bad offset/size"); return;
		}
		snprintf(dev, sizeof(dev), "/dev/%s", DISK_DEF);
	} else {
		snprintf(dev, sizeof(dev), "/dev/%sp%d", DISK_DEF,
			 (int)(p - P) + 1);			/* 行序即分区号 */
	}
	struct stat st;
	if (stat(tmp, &st) != 0 || (long long)st.st_size != p->size) {
		respond_text(c, 400, "tmp size mismatch"); return;
	}
	int in = open(tmp, O_RDONLY);
	if (in < 0) { respond_text(c, 500, "open tmp fail"); return; }
	int out = open(dev, O_WRONLY);
	if (out < 0) {
		close(in);
		char rs[128];
		snprintf(rs, sizeof(rs), "open %s: %s", dev, strerror(errno));
		respond_text(c, 500, rs);
		return;
	}
	if (raw_area && lseek(out, p->offset, SEEK_SET) < 0) {
		close(in); close(out);
		char rs[128];
		snprintf(rs, sizeof(rs), "lseek %lld: %s",
			 (long long)p->offset, strerror(errno));
		respond_text(c, 500, rs);
		return;
	}
	fprintf(stderr, "[ota-agent] commit %s → %s：dd %lldMiB 进行中…\n",
		name, dev, p->size >> 20);
	respond_stream_begin(c);
	char buf[65536];
	long long done = 0, last_rep = 0;
	int n;
	int wfail = 0;
	while ((n = (int)read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, (size_t)n) != n) { wfail = 1; break; }
		done += n;
		if (done - last_rep >= 16 * 1024 * 1024
		    || done == p->size) {
			last_rep = done;
			send_line(c, "part=%s stage=burn got=%lld total=%lld\n",
				  name, done, p->size);
		}
	}
	fsync(out);
	close(in);
	close(out);
	if (wfail || n < 0) {
		send_line(c, "fail write dev\n");
		fprintf(stderr, "[ota-agent] commit %s：write 失败\n", name);
		return;
	}
	/* 记录安装结果 + 版本，供 /status 对账 */
	state_set(p->name, p->sha256);
	snprintf(g_state_ver, sizeof(g_state_ver), "%s", g_man_ver);
	state_save();
	unlink(tmp);
	fprintf(stderr, "[ota-agent] %s → %s 完成（%lldB，ver %s）\n",
		p->name, dev, done, g_state_ver);
	send_line(c, "ok\n");
}

static void handle_slot(int c, const char *ab)
{
	if (strcmp(ab, "A") != 0 && strcmp(ab, "B") != 0) {
		respond_text(c, 400, "slot must be A|B");
		return;
	}
	int rc = setslot(ab);
	char rs[32];
	snprintf(rs, sizeof(rs), "ok rc=%d", rc);
	respond(c, 200, "text/plain", rs, (long)strlen(rs));
}

/* ---------- 连接处理 ---------- */
static void handle_conn(int c)
{
	char buf[HDR_MAX];
	int total = 0;
	struct req r;
	int hdrend = read_head(c, buf, sizeof(buf), &total, &r);
	if (hdrend < 0) {
		respond_text(c, 400, "bad request");
		return;
	}
	int is_get = strcmp(r.method, "GET") == 0;
	int is_post = strcmp(r.method, "POST") == 0;

	if (is_get && strcmp(r.path, "/status") == 0) {
		handle_status(c);
	} else if (is_get && strcmp(r.path, "/query") == 0) {
		handle_query(c);
	} else if (is_post && strcmp(r.path, "/manifest") == 0) {
		char *b = recv_body_mem(c, buf, hdrend, total,
					r.content_length, BODY_MAX);
		if (!b) { respond_text(c, 413, "body too large"); return; }
		handle_manifest(c, b, r.content_length);
		free(b);
	} else if (is_post && strcmp(r.path, "/select") == 0) {
		char *b = recv_body_mem(c, buf, hdrend, total,
					r.content_length, BODY_MAX);
		if (!b) { respond_text(c, 413, "body too large"); return; }
		handle_select(c, b);
		free(b);
	} else if (is_post && strncmp(r.path, "/part/", 6) == 0) {
		handle_part(c, &r, r.path + 6, buf, hdrend, total);
	} else if ((is_get || is_post) && strncmp(r.path, "/verify/", 8) == 0) {
		/* GET/POST 都收：流式响应，手动 wget（GET）与 PC 客户端（Get+流式）通吃 */
		handle_verify(c, r.path + 8);
	} else if ((is_get || is_post) && strncmp(r.path, "/commit/", 8) == 0) {
		handle_commit(c, r.path + 8);
	} else if (is_post && strncmp(r.path, "/slot/", 6) == 0) {
		handle_slot(c, r.path + 6);
	} else if (is_post && strcmp(r.path, "/abort") == 0) {
		clear_tmp();
		respond_text(c, 200, "ok");
	} else {
		respond_text(c, 404, "not found");
	}
}

/* ---------- daemon 模式 ---------- */
static int daemon_mode(int port)
{
	/* 后台化（OTA_AGENT_FG=1 时前台跑，便于调试）。
	 * 日志打到 /dev/console（串口可见）——烧写/校验是长静默操作，
	 * 板端无日志=不可观测（2026-10-04 教训）；无 console 时退 /dev/null */
	if (!getenv("OTA_AGENT_FG")) {
		pid_t pid = fork();
		if (pid < 0) die("fork");
		if (pid > 0) {
			printf("[ota-agent] daemon pid %d :%d\n", pid, port);
			return 0;
		}
		setsid();
		int cons = open("/dev/console", O_WRONLY);
		if (cons < 0)
			cons = open("/dev/null", O_WRONLY);
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0) dup2(nul, 0);
		if (cons >= 0) { dup2(cons, 1); dup2(cons, 2); }
		if (cons > 2) close(cons);
		if (nul > 2) close(nul);
	}
	signal(SIGPIPE, nop);

	FILE *pf = fopen(PID_FILE, "w");
	if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }

	state_load();			/* /data 没挂上也不退出，逐请求再试 */
	mkdir(OTA_DIR, 0755);

	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) die("socket");
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = INADDR_ANY;
	a.sin_port = htons((uint16_t)port);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 8) < 0)
		die("bind/listen 失败");
	fprintf(stderr, "[ota-agent] daemon :%d（status/query/manifest/select/"
		"part/verify/commit/slot/abort）\n", port);

	for (;;) {
		int c = accept(s, NULL, NULL);
		if (c < 0) continue;
		struct timeval tv = { .tv_sec = 60, .tv_usec = 0 };
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		handle_conn(c);
		close(c);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) goto usage;
	if (strcmp(argv[1], "daemon") == 0)
		return daemon_mode(argc > 2 ? atoi(argv[2]) : DEF_PORT);
	if (strcmp(argv[1], "query") == 0)
		return query_print(argc > 2 ? argv[2] : DISK_DEF);
	if (strcmp(argv[1], "setslot") == 0 && argc == 3) {
		int rc = setslot(argv[2]);
		printf("fw_setenv rc=%d\n", rc);
		return rc ? 1 : 0;
	}
usage:
	fprintf(stderr,
		"用法: ota-agent daemon [port] | query [disk] | setslot A|B\n");
	return 1;
}
