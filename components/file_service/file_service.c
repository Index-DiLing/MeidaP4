// SPDX-License-Identifier: Apache-2.0
//
// MeidaP4 · FILE 通道设备侧服务（协议 docs/protocol.md §5.3）
//
// ============================================================================
// 语义权威 = `web/file.js`（主机侧）。本文件按它的每个函数逐一对齐：
//
//   op  名字        请求 body                                  响应 body
//   --  ----------  ----------------------------------------  -----------------------------
//    0  LIST_REQ    char path[] \0                             ——
//    1  LIST_RSP    ——                                          N × {u32 size,u8 is_dir,name[256]}
//    2  OPEN_REQ    u8 mode + char path[] \0                    ——
//    3  OPEN_RSP    ——                                          u32 handle,u32 size,u8 is_dir (9 B)
//    4  READ_REQ    u32 handle,u32 length（offset 在 8 B 头里）  ——
//    5  READ_RSP    ——                                          数据（0 字节 = EOF，status=OK）
//    6  WRITE_REQ   u32 handle + 数据（≤1012；offset 在头里）    ——
//    7  WRITE_RSP   ——                                          u32 written
//    8  CLOSE_REQ   u32 handle（0xFFFFFFFF = 关全部）            ——
//    9  CLOSE_RSP   ——                                          空
//   10  DELETE      char path[] \0                              **原样回 op=10**
//   11  MKDIR       char path[] \0                              **原样回 op=11**
//   12  RENAME      char src[] \0 char dst[] \0                 **原样回 op=12**
//
// 三条最容易踩的约定（web/file.js 里都能读到）：
//   ① LIST_RSP 单条 261 B，body 上限 1016 B ⇒ **一页最多 3 条**；返回 0 条 = 目录结束。
//      翻页用 8 字节头里的 **offset 字段 = 起始条目序号**（不是字节偏移）。
//   ② OPEN_RSP **9 字节**（decodeOpenRsp 只要求 ≥9；多写 rsv 反而无益）。
//   ③ DELETE/MKDIR/RENAME 没有独立 RSP opcode ⇒ **设备原样回同一个 op**，靠 xid 配对。
//
// ============================================================================
// 线程模型（为什么 handler 里**不**做文件 I/O）
//
//   `webusb_proto_set_file_handler()` 注册的回调运行在 **wu_proto 任务**里（栈 8 KB，
//   优先级 5）。那个任务同时负责 ACK / 200 ms 重传 / HELLO 状态机 —— 一旦 FATFS 操作
//   （尤其 listing 大目录、SD 写扇区）在那里阻塞几十毫秒，整个可靠通道的时序都会被拖。
//
//   所以这里分成两段：
//     wu_proto 任务 : fs_on_file_msg() —— 只把整条消息（8 B 头 + body）拷进请求队列
//     fs_svc  任务  : fs_svc_task()   —— 真正的 FATFS 操作 + webusb_proto_send_file_reply()
//
//   两条硬约束因此成立：
//     * **句柄表（FILE*）只被 fs_svc 任务碰**，不跨任务共享 FILE*；
//     * 请求队列满时立刻回 EBUSY（绝不静默吞请求 —— 主机有 2 s 超时，吞了只会更难查）。
//
//   与 MTP 的并发：两者都走同一套 VFS，`CONFIG_FATFS_LINK_LOCK=y` 给卷级互斥
//   （`vfs_fat.c` 的 `fat_ctx->lock`），所以并发访问是安全的 —— 这也正是"文件级共享"
//   能成立的前提。
//
// ============================================================================
// 路径解析（安全边界）
//
//   根 = `sd_storage_mount_point()`（当前 "/sdcard"）。接受：
//       ""、"/"、"/foo.txt"、"/sdcard/foo.txt"、"foo.txt"（相对）、反斜杠会被归一成 '/'。
//   拒绝：任何 ".." 组件、控制字符、单组件 > 255 B、归一化后总长 ≥ 320 B。
//   拼出来的路径**永远**以根为前缀 —— 结构上不可能逃出 /sdcard。
//
// 作者：WS-F（TF 卡共享）。写作用域：components/file_service/**。

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "sd_storage.h"
#include "webusb_proto.h"
#include "wu_codec.h"

#include "file_service.h"
#include "file_service_internal.h"

static const char *TAG = "file_service";

/* ============================ 编译期常量 ================================== */

/** 单帧 payload 上限（协议 §2.1） */
#define FS_PAYLOAD_MAX          WU_MAX_PAYLOAD            /* 1024 */
/** LIST_RSP 单个目录项：u32 size + u8 is_dir + char name[256] */
#define FS_ENTRY_LEN            (4 + 1 + FILE_SERVICE_NAME_LEN)   /* 261 */
/** LIST_RSP 一页最多几条：floor((1024-8)/261) = 3（3×261 = 783 ≤ 1016） */
#define FS_LIST_MAX_ENTRIES     ((FS_PAYLOAD_MAX - WU_FILE_HDR_LEN) / FS_ENTRY_LEN)
/** READ_RSP 单帧可带的数据字节：1024 - 8 = 1016 */
#define FS_READ_MAX             (FS_PAYLOAD_MAX - WU_FILE_HDR_LEN)
/** WRITE_REQ 单帧可带的数据字节：1024 - 8 - 4 = 1012 */
#define FS_WRITE_MAX            (FS_PAYLOAD_MAX - WU_FILE_HDR_LEN - 4)
/** OPEN_RSP body：u32 handle + u32 size + u8 is_dir（web/file.js 只要求 ≥ 9） */
#define FS_OPEN_RSP_LEN         9
/** 归一化后的完整路径缓冲（含根前缀） */
#define FS_PATH_MAX             320
/** FATFS LFN 上限（CONFIG_FATFS_MAX_LFN=255）；这里按 **UTF-8 字节**保守限制 */
#define FS_COMP_MAX             255
/** CLOSE_REQ 的"关全部"魔数 */
#define FS_CLOSE_ALL            0xFFFFFFFFu

/** 请求队列深度（主机侧 FileClient 是串行 await，窗口 8；8 深足够吸收突发） */
#define FS_REQ_QDEPTH           8
/** FILE 服务任务（只做 FATFS I/O） */
#define FS_SVC_TASK_STACK       6144
#define FS_SVC_TASK_PRIO        4
/** 链路"代"变化时关闭全部句柄：轮询周期 */
#define FS_SVC_IDLE_MS          100

/* ============================ 共享统计 ==================================== */

/** 统计唯一实例（内部头里 extern 给探针任务用） */
file_service_stats_t fs_shared_stats;

/** 只用来自旋保护统计（**绝不**在临界区里做 I/O） */
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

void fs_stats_begin(void)
{
    taskENTER_CRITICAL(&s_stats_mux);
}

void fs_stats_end(void)
{
    taskEXIT_CRITICAL(&s_stats_mux);
}

/* ============================ 状态 ======================================== */

/** 一条待处理的 FILE 请求（8 B 头 + body，最大 1024 B） */
typedef struct {
    uint32_t len;                   /**< 实际长度（≤ WU_MAX_PAYLOAD）；用 u32 保证 4 字节对齐 */
    uint8_t  msg[WU_MAX_PAYLOAD];   /**< 协议层已保证 ≤ 1024 */
} fs_req_t;

/** handler 的拷贝中转（**只有 wu_proto 任务碰**，所以静态即可，省 8 KB 任务栈） */
static fs_req_t s_scratch;

static uint8_t *s_req_storage;             /**< 队列存储（优先 PSRAM） */
static bool     s_req_storage_is_psram;
static StaticQueue_t s_req_qcb;
static QueueHandle_t s_reqq;

static bool s_started;

typedef struct {
    bool     used;
    bool     is_dir;
    uint8_t  gen;      /**< 句柄代，防止"关闭后又复用"的迟到请求误伤新文件 */
    FILE    *fp;       /**< 目录句柄为 NULL（LIST 自己 opendir，不需要持久 DIR*） */
} fs_handle_t;

/** 句柄表 —— **只被 fs_svc 任务访问**（不跨任务共享 FILE*） */
static fs_handle_t s_handles[FILE_SERVICE_MAX_HANDLES];

/* ============================ 小工具 ====================================== */

static size_t fs_root_len(void)
{
    const char *root = sd_storage_mount_point();
    return (root == NULL) ? 0 : strlen(root);
}

const char *file_service_root(void)
{
    return sd_storage_mount_point();
}

bool file_service_is_started(void)
{
    return s_started;
}

/** 请求 op → 响应 op（10/11/12 没有独立 RSP，原样回；见 web/file.js 头部表格） */
static uint8_t fs_rsp_op(uint8_t req_op)
{
    switch (req_op) {
    case WU_FILE_LIST_REQ:  return WU_FILE_LIST_RSP;
    case WU_FILE_OPEN_REQ:  return WU_FILE_OPEN_RSP;
    case WU_FILE_READ_REQ:  return WU_FILE_READ_RSP;
    case WU_FILE_WRITE_REQ: return WU_FILE_WRITE_RSP;
    case WU_FILE_CLOSE_REQ: return WU_FILE_CLOSE_RSP;
    default:                return req_op;   /* DELETE / MKDIR / RENAME */
    }
}

/** errno → 协议 §5.1 状态码 */
static uint8_t fs_status_from_errno(int err)
{
    switch (err) {
    case 0:            return WU_ST_OK;
    case ENOENT:       return WU_ST_ENOENT;
    case ENOSPC:
    case ENOMEM:
    case EFBIG:        return WU_ST_ENOSPC;
    case EBUSY:
    case EACCES:
    case EPERM:
    case ENOTEMPTY:
    case EEXIST:       return WU_ST_EBUSY;
    case EINVAL:
    case ENOTDIR:
    case EISDIR:
    case ENAMETOOLONG: return WU_ST_EINVAL;
    case ENOTSUP:      return WU_ST_ENOTSUP;
    case EIO:
    default:           return WU_ST_EIO;
    }
}

/**
 * @brief 回一条 FILE 响应（可从 fs_svc / wu_proto 两个任务调用）。
 *
 * @param wait_for_txq true  = TX 队列满时最多重试 ~500 ms（fs_svc 任务用：慢一点没关系）
 *                     false = 只试一次（**handler 用**：绝不能把 wu_proto 任务堵在这里，
 *                             那是 ACK/重传/HELLO 的线程）
 */
static void fs_reply_ex(uint8_t rsp_op, uint8_t status, uint16_t xid, uint32_t offset,
                        const void *body, size_t len, bool wait_for_txq)
{
    esp_err_t err;
    int tries = 0;

    for (;;) {
        err = webusb_proto_send_file_reply(rsp_op, status, xid, offset, body, len);
        if (err == ESP_OK) {
            break;
        }
        /* 只有"队列满"值得重试；未启动/参数错重试也没用 */
        if (!wait_for_txq || err != ESP_ERR_NO_MEM || ++tries >= 50) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    fs_stats_begin();
    if (err == ESP_OK) {
        fs_shared_stats.responses++;
    } else {
        fs_shared_stats.reply_dropped++;
    }
    if (status != WU_ST_OK) {
        fs_shared_stats.errors++;
    }
    fs_stats_end();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "回包失败 op=%u status=0x%02X xid=%u err=%s（主机侧会超时）",
                 (unsigned)rsp_op, (unsigned)status, (unsigned)xid, esp_err_to_name(err));
    }
}

static void fs_reply(uint8_t rsp_op, uint8_t status, uint16_t xid, uint32_t offset,
                     const void *body, size_t len)
{
    fs_reply_ex(rsp_op, status, xid, offset, body, len, true);
}

/** 纯路径类请求（DELETE/MKDIR/RENAME）的错误回执：原样回 op */
static void fs_reply_path_err(uint8_t op, uint8_t status, uint16_t xid)
{
    if (status == WU_ST_EINVAL) {
        fs_stats_begin();
        fs_shared_stats.path_rejected++;
        fs_stats_end();
    }
    fs_reply(op, status, xid, 0, NULL, 0);
}

/* ============================ 路径解析 ==================================== */

typedef struct {
    char   buf[FS_PATH_MAX];
    size_t len;             /**< 不含结尾 NUL */
} fs_path_t;

/**
 * @brief 把线上来的路径（UTF-8，NUL 结尾）归一化成**根限定**的 VFS 路径。
 *
 * @param body 路径字节（可以带尾随 NUL，也可以不带 —— 都接受）
 * @param blen body 可用长度（遇到 NUL 提前结束）
 * @param out  输出
 * @return WU_ST_OK 或错误码（EINVAL 路径非法 / EIO 根配置异常）
 */
static uint8_t fs_path_resolve(const uint8_t *body, size_t blen, fs_path_t *out)
{
    const char *root = sd_storage_mount_point();
    char   raw[FS_PATH_MAX];
    size_t root_len;
    size_t n = 0;
    size_t i;
    const char *rest;
    const char *p;

    if (root == NULL || out == NULL) {
        return WU_ST_EIO;
    }
    root_len = strlen(root);
    if (root_len == 0 || root_len > (FS_PATH_MAX - 4)) {
        return WU_ST_EIO;
    }
    if (body == NULL || blen == 0) {
        return WU_ST_EINVAL;
    }

    /* 1) 拷贝到本地 + 归一化：'\' → '/'，拒绝控制字符（含内嵌 NUL 之后的垃圾不管） */
    for (i = 0; i < blen; i++) {
        char c = (char)body[i];
        if (c == '\0') {
            break;
        }
        if ((unsigned char)c < 0x20) {
            return WU_ST_EINVAL;
        }
        if (c == '\\') {
            c = '/';
        }
        if (n + 1 >= sizeof(raw)) {     /* 超长：拒绝，不截断（截断会指向另一个文件） */
            return WU_ST_EINVAL;
        }
        raw[n++] = c;
    }
    raw[n] = '\0';

    if (n == 0) {                        /* 空路径 = 根 */
        memcpy(out->buf, root, root_len + 1);
        out->len = root_len;
        return WU_ST_OK;
    }

    /* 2) 已经是"根内绝对路径"就直接用；否则当相对路径拼到根上。
          ⚠️ 只认**整段**前缀："/sdcardx" 不会被误判成根内路径。 */
    if (strncmp(raw, root, root_len) == 0 && (raw[root_len] == '\0' || raw[root_len] == '/')) {
        rest = raw + root_len;
    } else {
        rest = raw;
    }

    /* 3) 逐组件重建：跳过空组件与 "."，拒绝 ".."，限制单组件长度 */
    memcpy(out->buf, root, root_len);
    out->len = root_len;
    p = rest;
    while (*p != '\0') {
        const char *slash;
        size_t clen;

        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        slash = strchr(p, '/');
        clen = (slash != NULL) ? (size_t)(slash - p) : strlen(p);

        if (clen == 1 && p[0] == '.') {          /* "." 直接丢掉 */
            p += clen;
            continue;
        }
        if (clen == 2 && p[0] == '.' && p[1] == '.') {
            return WU_ST_EINVAL;                 /* ⚠️ 唯一能逃出根的入口，一律拒绝 */
        }
        if (clen > FS_COMP_MAX) {
            return WU_ST_EINVAL;
        }
        if (out->len + 1 + clen + 1 > sizeof(out->buf)) {
            return WU_ST_EINVAL;
        }
        out->buf[out->len++] = '/';
        memcpy(out->buf + out->len, p, clen);
        out->len += clen;
        p += clen;
    }
    out->buf[out->len] = '\0';
    return WU_ST_OK;
}

/** 把一个目录项名字拷进 256 B 定长域（不切断 UTF-8 序列） */
static void fs_copy_name(uint8_t *dst, const char *src)
{
    size_t n = strlen(src);

    if (n > (size_t)(FILE_SERVICE_NAME_LEN - 1)) {
        n = (size_t)(FILE_SERVICE_NAME_LEN - 1);
        while (n > 0 && ((uint8_t)src[n] & 0xC0u) == 0x80u) {
            n--;    /* 回退到 code point 边界，不产生半个汉字 */
        }
    }
    memcpy(dst, src, n);    /* dst 已 memset 0，尾部天然 NUL */
}

/* ============================ 句柄表 ====================================== */

static fs_handle_t *fs_handle_lookup(uint32_t handle)
{
    uint32_t slot = handle & 0xFFu;
    uint8_t  gen = (uint8_t)(handle >> 8);
    fs_handle_t *h;

    if (slot == 0 || slot > FILE_SERVICE_MAX_HANDLES) {
        return NULL;
    }
    h = &s_handles[slot - 1];
    if (!h->used || h->gen != gen) {
        return NULL;
    }
    return h;
}

/** @return 句柄（非 0）；0 = 句柄表满 */
static uint32_t fs_handle_alloc(FILE *fp, bool is_dir)
{
    int i;

    for (i = 0; i < FILE_SERVICE_MAX_HANDLES; i++) {
        fs_handle_t *h = &s_handles[i];

        if (h->used) {
            continue;
        }
        h->used = true;
        h->fp = fp;
        h->is_dir = is_dir;
        h->gen = (uint8_t)(h->gen + 1u);
        if (h->gen == 0) {
            h->gen = 1;
        }
        fs_stats_begin();
        fs_shared_stats.handles_open++;
        fs_stats_end();
        return ((uint32_t)h->gen << 8) | (uint32_t)(i + 1);
    }
    return 0;
}

static void fs_handle_release(fs_handle_t *h)
{
    if (h == NULL || !h->used) {
        return;
    }
    if (h->fp != NULL) {
        (void)fclose(h->fp);
        h->fp = NULL;
    }
    h->used = false;
    h->is_dir = false;
    fs_stats_begin();
    if (fs_shared_stats.handles_open > 0) {
        fs_shared_stats.handles_open--;
    }
    fs_stats_end();
}

static void fs_close_all_handles(const char *why)
{
    int i;
    uint32_t closed = 0;

    for (i = 0; i < FILE_SERVICE_MAX_HANDLES; i++) {
        if (s_handles[i].used) {
            fs_handle_release(&s_handles[i]);
            closed++;
        }
    }
    if (closed != 0) {
        ESP_LOGW(TAG, "关闭全部句柄 %u 个（%s）—— 主机会话已重置，旧句柄不再有效",
                 (unsigned)closed, (why == NULL) ? "?" : why);
    }
}

/* ============================ 各 op 实现 ================================== */

/** op 0：LIST_REQ → LIST_RSP（offset = 起始条目序号；一页最多 3 条） */
static void fs_do_list(const uint8_t *body, size_t blen, uint16_t xid, uint32_t offset)
{
    fs_path_t path;
    DIR *dir;
    struct dirent *de;
    uint8_t out[FS_LIST_MAX_ENTRIES * FS_ENTRY_LEN];
    size_t used = 0;
    uint32_t seen = 0;
    int entries = 0;
    uint8_t st = fs_path_resolve(body, blen, &path);

    if (st != WU_ST_OK) {
        fs_reply_path_err(WU_FILE_LIST_REQ, st, xid);
        return;
    }

    dir = opendir(path.buf);
    if (dir == NULL) {
        st = fs_status_from_errno(errno);
        ESP_LOGW(TAG, "LIST 打不开 '%s': errno=%d", path.buf, errno);
        fs_reply(WU_FILE_LIST_RSP, st, xid, offset, NULL, 0);
        return;
    }

    while ((de = readdir(dir)) != NULL) {
        char full[FS_PATH_MAX];
        struct stat sb;
        bool is_dir;
        uint32_t size = 0;
        size_t plen = path.len;
        size_t nlen = strlen(de->d_name);
        uint8_t *e;

        /* FatFs 在 FF_FS_RPATH=0 的配置下会把 "." / ".." 也返回；**先过滤再计数**，
           否则翻页的 offset 语义会随配置而变。 */
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (seen < offset) {    /* 翻页：跳过前 offset 条（已过滤） */
            seen++;
            continue;
        }
        if (entries >= FS_LIST_MAX_ENTRIES) {
            break;
        }

        /* readdir 的 d_type 不给大小、且 FATFS 上 d_type 未必可靠 ⇒ 每条 ent 都 stat 一次。
           路径用显式长度拼接（不用 snprintf：截断必须被当成错误，而不是被格式化库悄悄吃掉）。 */
        is_dir = (de->d_type == DT_DIR);
        if (plen + 1 + nlen + 1 <= sizeof(full)) {
            memcpy(full, path.buf, plen);
            full[plen] = '/';
            memcpy(full + plen + 1, de->d_name, nlen + 1);
            if (stat(full, &sb) == 0) {
                is_dir = S_ISDIR(sb.st_mode);
                if (!is_dir && sb.st_size > 0) {
                    size = (uint32_t)sb.st_size;
                }
            }
        }

        e = out + used;
        memset(e, 0, FS_ENTRY_LEN);
        wu_wr32(e, size);
        e[4] = is_dir ? 1u : 0u;
        fs_copy_name(e + 5, de->d_name);
        used += FS_ENTRY_LEN;
        entries++;
    }
    (void)closedir(dir);

    fs_stats_begin();
    fs_shared_stats.list_pages++;
    fs_stats_end();

    /* entries == 0 ⇒ 目录已列完（主机据此停止翻页） */
    fs_reply(WU_FILE_LIST_RSP, WU_ST_OK, xid, offset, (used != 0) ? out : NULL, used);
}

/** op 2：OPEN_REQ → OPEN_RSP（9 B：u32 handle,u32 size,u8 is_dir） */
static void fs_do_open(const uint8_t *body, size_t blen, uint16_t xid)
{
    fs_path_t path;
    struct stat sb;
    bool have_stat = false;
    bool is_dir = false;
    uint8_t mode;
    uint8_t st;
    FILE *fp = NULL;
    uint32_t size = 0;
    uint32_t handle;
    uint8_t rsp[FS_OPEN_RSP_LEN];

    if (blen < 2) {             /* mode + 至少一个字符（根可用 "/" 表示） */
        fs_reply_path_err(WU_FILE_OPEN_REQ, WU_ST_EINVAL, xid);
        return;
    }
    mode = body[0];
    if (mode > 1) {
        fs_reply_path_err(WU_FILE_OPEN_REQ, WU_ST_EINVAL, xid);
        return;
    }
    st = fs_path_resolve(body + 1, blen - 1, &path);
    if (st != WU_ST_OK) {
        fs_reply_path_err(WU_FILE_OPEN_REQ, st, xid);
        return;
    }

    if (stat(path.buf, &sb) == 0) {
        have_stat = true;
        is_dir = S_ISDIR(sb.st_mode);
    }

    if (mode == 0) {
        /* 只读打开。目录也允许 open 成功（is_dir=1）：web/file.js 的 readFile()
           先 open 再判断 isDir，然后自己报错并 close —— 我们照它的流程来。 */
        if (have_stat && is_dir) {
            handle = fs_handle_alloc(NULL, true);
            size = 0;
        } else {
            fp = fopen(path.buf, "rb");
            if (fp == NULL) {
                st = fs_status_from_errno(errno);
                ESP_LOGW(TAG, "OPEN(r) 失败 '%s': errno=%d", path.buf, errno);
                fs_reply(WU_FILE_OPEN_RSP, st, xid, 0, NULL, 0);
                return;
            }
            if (have_stat) {
                size = (sb.st_size > 0) ? (uint32_t)sb.st_size : 0;
            }
            handle = fs_handle_alloc(fp, false);
        }
    } else {
        /* 写/创建/截断。目录不能按文件写。 */
        if (have_stat && is_dir) {
            fs_reply(WU_FILE_OPEN_RSP, WU_ST_EINVAL, xid, 0, NULL, 0);
            return;
        }
        /* "w+b"：创建/截断 + 可读 —— 主机写完不关句柄就能回读自检（web/file.js 不依赖，但没坏处） */
        fp = fopen(path.buf, "w+b");
        if (fp == NULL) {
            st = fs_status_from_errno(errno);
            ESP_LOGW(TAG, "OPEN(w) 失败 '%s': errno=%d", path.buf, errno);
            fs_reply(WU_FILE_OPEN_RSP, st, xid, 0, NULL, 0);
            return;
        }
        size = 0;
        handle = fs_handle_alloc(fp, false);
    }

    if (handle == 0) {
        if (fp != NULL) {
            (void)fclose(fp);
        }
        ESP_LOGW(TAG, "句柄表已满（%d）—— 主机没有 CLOSE 掉的句柄太多", FILE_SERVICE_MAX_HANDLES);
        fs_reply(WU_FILE_OPEN_RSP, WU_ST_EBUSY, xid, 0, NULL, 0);
        return;
    }

    wu_wr32(rsp, handle);
    wu_wr32(rsp + 4, size);
    rsp[8] = is_dir ? 1u : 0u;
    fs_reply(WU_FILE_OPEN_RSP, WU_ST_OK, xid, 0, rsp, sizeof(rsp));
}

/** op 4：READ_REQ → READ_RSP（offset 在 8 B 头里；0 字节 + OK = EOF） */
static void fs_do_read(const uint8_t *body, size_t blen, uint16_t xid, uint32_t offset)
{
    uint8_t buf[FS_READ_MAX];
    uint32_t req_len;
    uint32_t handle;
    fs_handle_t *h;
    size_t n;
    uint8_t st = WU_ST_OK;

    if (blen < 8) {
        fs_reply(WU_FILE_READ_RSP, WU_ST_EINVAL, xid, offset, NULL, 0);
        return;
    }
    handle = wu_rd32(body);
    req_len = wu_rd32(body + 4);
    if (req_len == 0) {
        fs_reply(WU_FILE_READ_RSP, WU_ST_EINVAL, xid, offset, NULL, 0);
        return;
    }
    if (req_len > FS_READ_MAX) {
        req_len = FS_READ_MAX;      /* 契约上限；超出就截断，不拒绝（主机侧也会自己 cap） */
    }
    if (offset > (uint32_t)LONG_MAX) {
        /* fseek() 的偏移是 long（32 位）—— >2 GiB 的定位不支持，明确报错而不是悄悄读错位置 */
        fs_reply(WU_FILE_READ_RSP, WU_ST_EINVAL, xid, offset, NULL, 0);
        return;
    }

    h = fs_handle_lookup(handle);
    if (h == NULL || h->is_dir || h->fp == NULL) {
        fs_reply(WU_FILE_READ_RSP, WU_ST_EINVAL, xid, offset, NULL, 0);
        return;
    }
    if (fseek(h->fp, (long)offset, SEEK_SET) != 0) {
        clearerr(h->fp);
        fs_reply(WU_FILE_READ_RSP, WU_ST_EIO, xid, offset, NULL, 0);
        return;
    }

    n = fread(buf, 1, (size_t)req_len, h->fp);
    if (n < (size_t)req_len && ferror(h->fp) != 0) {
        clearerr(h->fp);
        st = WU_ST_EIO;
    }

    if (st == WU_ST_OK) {
        fs_stats_begin();
        fs_shared_stats.bytes_read += (uint32_t)n;
        fs_stats_end();
    }
    /* n == 0 且 status == OK ⇒ EOF（web/file.js 就是这么判的） */
    fs_reply(WU_FILE_READ_RSP, st, xid, offset, (n != 0) ? buf : NULL, n);
}

/** op 6：WRITE_REQ → WRITE_RSP（body = u32 handle + 数据 ≤1012；offset 在头里） */
static void fs_do_write(const uint8_t *body, size_t blen, uint16_t xid, uint32_t offset)
{
    uint32_t handle;
    size_t dlen;
    fs_handle_t *h;
    size_t n;
    uint8_t rsp[4];
    uint8_t st = WU_ST_OK;

    if (blen < 4) {
        fs_reply(WU_FILE_WRITE_RSP, WU_ST_EINVAL, xid, offset, NULL, 0);
        return;
    }
    handle = wu_rd32(body);
    dlen = blen - 4;
    if (dlen > FS_WRITE_MAX) {
        fs_reply(WU_FILE_WRITE_RSP, WU_ST_EINVAL, xid, offset, NULL, 0);
        return;
    }
    if (offset > (uint32_t)LONG_MAX) {
        wu_wr32(rsp, 0);
        fs_reply(WU_FILE_WRITE_RSP, WU_ST_EINVAL, xid, offset, rsp, sizeof(rsp));
        return;
    }

    h = fs_handle_lookup(handle);
    if (h == NULL || h->is_dir || h->fp == NULL) {
        wu_wr32(rsp, 0);
        fs_reply(WU_FILE_WRITE_RSP, WU_ST_EINVAL, xid, offset, rsp, sizeof(rsp));
        return;
    }
    if (fseek(h->fp, (long)offset, SEEK_SET) != 0) {
        clearerr(h->fp);
        wu_wr32(rsp, 0);
        fs_reply(WU_FILE_WRITE_RSP, WU_ST_EIO, xid, offset, rsp, sizeof(rsp));
        return;
    }

    n = fwrite(body + 4, 1, dlen, h->fp);
    if (n > 0) {
        /* 短写（< dlen）也回 OK + 实际字节数：web/file.js 的 writeFile() 会从
           written 处继续发，不会丢数据。真正写不进去（n == 0）才报错。 */
        fs_stats_begin();
        fs_shared_stats.bytes_written += (uint32_t)n;
        fs_stats_end();
    } else if (dlen != 0) {
        st = fs_status_from_errno(errno);
        if (st == WU_ST_OK) {
            st = WU_ST_EIO;
        }
        clearerr(h->fp);
        ESP_LOGW(TAG, "WRITE 失败 offset=%u len=%u errno=%d", (unsigned)offset,
                 (unsigned)dlen, errno);
    }

    wu_wr32(rsp, (uint32_t)n);
    fs_reply(WU_FILE_WRITE_RSP, st, xid, offset, rsp, sizeof(rsp));
}

/** op 8：CLOSE_REQ → CLOSE_RSP（handle == 0xFFFFFFFF 表示关全部） */
static void fs_do_close(const uint8_t *body, size_t blen, uint16_t xid)
{
    uint32_t handle;
    fs_handle_t *h;

    if (blen < 4) {
        fs_reply(WU_FILE_CLOSE_RSP, WU_ST_EINVAL, xid, 0, NULL, 0);
        return;
    }
    handle = wu_rd32(body);
    if (handle == FS_CLOSE_ALL) {
        fs_close_all_handles("CLOSE 0xFFFFFFFF");
        fs_reply(WU_FILE_CLOSE_RSP, WU_ST_OK, xid, 0, NULL, 0);
        return;
    }
    h = fs_handle_lookup(handle);
    if (h == NULL) {
        /* 未知/已关句柄：EINVAL。web/file.js 在 readFile/writeFile 的 finally 里
           用 .catch(()=>{}) 兜住，所以不会把 UI 带崩。 */
        fs_reply(WU_FILE_CLOSE_RSP, WU_ST_EINVAL, xid, 0, NULL, 0);
        return;
    }
    fs_handle_release(h);
    fs_reply(WU_FILE_CLOSE_RSP, WU_ST_OK, xid, 0, NULL, 0);
}

/** op 10：DELETE（原样回 op=10） */
static void fs_do_delete(const uint8_t *body, size_t blen, uint16_t xid)
{
    fs_path_t path;
    uint8_t st = fs_path_resolve(body, blen, &path);

    if (st != WU_ST_OK) {
        fs_reply_path_err(WU_FILE_DELETE, st, xid);
        return;
    }
    if (path.len <= fs_root_len()) {        /* 根目录不许删 */
        fs_reply(WU_FILE_DELETE, WU_ST_EINVAL, xid, 0, NULL, 0);
        return;
    }
    if (remove(path.buf) != 0) {
        st = fs_status_from_errno(errno);
        ESP_LOGW(TAG, "DELETE 失败 '%s': errno=%d", path.buf, errno);
        fs_reply(WU_FILE_DELETE, st, xid, 0, NULL, 0);
        return;
    }
    fs_reply(WU_FILE_DELETE, WU_ST_OK, xid, 0, NULL, 0);
}

/** op 11：MKDIR（原样回 op=11；已存在且是目录 ⇒ 幂等回 OK） */
static void fs_do_mkdir(const uint8_t *body, size_t blen, uint16_t xid)
{
    fs_path_t path;
    uint8_t st = fs_path_resolve(body, blen, &path);

    if (st != WU_ST_OK) {
        fs_reply_path_err(WU_FILE_MKDIR, st, xid);
        return;
    }
    if (path.len <= fs_root_len()) {        /* 根已存在 */
        fs_reply(WU_FILE_MKDIR, WU_ST_OK, xid, 0, NULL, 0);
        return;
    }
    if (mkdir(path.buf, 0777) != 0) {
        if (errno == EEXIST) {
            struct stat sb;
            if (stat(path.buf, &sb) == 0 && S_ISDIR(sb.st_mode)) {
                fs_reply(WU_FILE_MKDIR, WU_ST_OK, xid, 0, NULL, 0);  /* 幂等 */
                return;
            }
        }
        st = fs_status_from_errno(errno);
        ESP_LOGW(TAG, "MKDIR 失败 '%s': errno=%d", path.buf, errno);
        fs_reply(WU_FILE_MKDIR, st, xid, 0, NULL, 0);
        return;
    }
    fs_reply(WU_FILE_MKDIR, WU_ST_OK, xid, 0, NULL, 0);
}

/** op 12：RENAME（body = src \0 dst \0；原样回 op=12） */
static void fs_do_rename(const uint8_t *body, size_t blen, uint16_t xid)
{
    fs_path_t src;
    fs_path_t dst;
    size_t nul_at = blen;
    size_t i;
    uint8_t st;

    for (i = 0; i < blen; i++) {
        if (body[i] == 0) {
            nul_at = i;
            break;
        }
    }
    if (nul_at == blen || nul_at + 1 >= blen) {    /* 必须有两个 NUL 结尾的串 */
        fs_reply_path_err(WU_FILE_RENAME, WU_ST_EINVAL, xid);
        return;
    }
    st = fs_path_resolve(body, nul_at + 1, &src);
    if (st != WU_ST_OK) {
        fs_reply_path_err(WU_FILE_RENAME, st, xid);
        return;
    }
    st = fs_path_resolve(body + nul_at + 1, blen - nul_at - 1, &dst);
    if (st != WU_ST_OK) {
        fs_reply_path_err(WU_FILE_RENAME, st, xid);
        return;
    }
    if (src.len <= fs_root_len() || dst.len <= fs_root_len()) {   /* 根目录不许改名/被覆盖 */
        fs_reply(WU_FILE_RENAME, WU_ST_EINVAL, xid, 0, NULL, 0);
        return;
    }
    if (rename(src.buf, dst.buf) != 0) {
        st = fs_status_from_errno(errno);   /* 目标已存在 ⇒ EEXIST ⇒ WU_ST_EBUSY */
        ESP_LOGW(TAG, "RENAME 失败 '%s' → '%s': errno=%d", src.buf, dst.buf, errno);
        fs_reply(WU_FILE_RENAME, st, xid, 0, NULL, 0);
        return;
    }
    fs_reply(WU_FILE_RENAME, WU_ST_OK, xid, 0, NULL, 0);
}

/* ============================ 派发 ======================================== */

/** 在 fs_svc 任务上下文处理一条完整请求（8 B 头 + body） */
static void fs_dispatch(const fs_req_t *req)
{
    const uint8_t *msg = req->msg;
    const uint8_t *body;
    size_t blen;
    uint8_t op;
    uint16_t xid;
    uint32_t offset;

    if (req->len < (uint32_t)WU_FILE_HDR_LEN) {
        return;
    }
    op = msg[0];
    xid = wu_rd16(msg + 2);
    offset = wu_rd32(msg + 4);
    body = msg + WU_FILE_HDR_LEN;
    blen = (size_t)req->len - (size_t)WU_FILE_HDR_LEN;

    /* op 是 uint8_t 且 WU_FILE_LIST_REQ == 0 ⇒ 下界恒成立，只查上界
       （写 `op < WU_FILE_LIST_REQ` 会被 -Wtype-limits/-Werror 直接判死）。 */
    if (op > (uint8_t)WU_FILE_RENAME) {
        fs_stats_begin();
        fs_shared_stats.rx_ignored++;
        fs_stats_end();
        ESP_LOGW(TAG, "忽略未知 FILE op=%u xid=%u", (unsigned)op, (unsigned)xid);
        return;
    }
    if (op != (uint8_t)WU_FILE_MKDIR && (op % 2) != 0) {
        /* 主机发来的是"响应"类 opcode（1/3/5/7/9）—— 协议上不该出现，静默丢弃并计数，
           回一个响应会很危险（会与真实响应撞 xid）。
           ⚠️ 2026-10-22 T2 真机修正：**MKDIR(11) 不是响应类**，它是单 op 语义
           （DELETE=10 / MKDIR=11 / RENAME=12，见 docs/protocol.md §5.3 与 web/file.js）。
           旧条件 `(op%2)!=0` 把 MKDIR 一起吞了 ⇒ 主机侧 MKDIR 永远超时。 */
        fs_stats_begin();
        fs_shared_stats.rx_ignored++;
        fs_stats_end();
        ESP_LOGW(TAG, "忽略主机误发的响应类 op=%u xid=%u", (unsigned)op, (unsigned)xid);
        return;
    }

    if (!sd_storage_is_mounted()) {
        /* 失败策略：服务照常活着，请求统一回 EBUSY（不崩、不阻塞 USB） */
        fs_stats_begin();
        fs_shared_stats.unmounted++;
        fs_stats_end();
        fs_reply(fs_rsp_op(op), WU_ST_EBUSY, xid, offset, NULL, 0);
        return;
    }

    switch (op) {
    case WU_FILE_LIST_REQ:  fs_do_list(body, blen, xid, offset);     break;
    case WU_FILE_OPEN_REQ:  fs_do_open(body, blen, xid);             break;
    case WU_FILE_READ_REQ:  fs_do_read(body, blen, xid, offset);     break;
    case WU_FILE_WRITE_REQ: fs_do_write(body, blen, xid, offset);    break;
    case WU_FILE_CLOSE_REQ: fs_do_close(body, blen, xid);            break;
    case WU_FILE_DELETE:    fs_do_delete(body, blen, xid);           break;
    case WU_FILE_MKDIR:     fs_do_mkdir(body, blen, xid);            break;
    case WU_FILE_RENAME:    fs_do_rename(body, blen, xid);           break;
    default:
        /* 走到这里只可能是上面 (op % 2) 过滤漏掉的偶数未知 op，防御性回 EINVAL */
        fs_reply(op, WU_ST_EINVAL, xid, offset, NULL, 0);
        break;
    }
}

/* ============================ 协议入口（wu_proto 任务） ==================== */

/**
 * @brief webusb_proto 的 FILE handler —— **运行在 wu_proto 任务里**。
 *
 * 只做三件事：校验长度 → 拷贝进请求队列 → 返回。真正的 I/O 在 fs_svc 任务。
 * 队列满时立刻回 EBUSY（不阻塞、不吞请求）。
 */
static void fs_on_file_msg(const uint8_t *msg, size_t len, void *ctx)
{
    (void)ctx;
    if (msg == NULL || len < (size_t)WU_FILE_HDR_LEN || len > sizeof(s_scratch.msg)) {
        return;
    }

    {
        uint8_t op = msg[0];
        fs_stats_begin();
        fs_shared_stats.requests++;
        if (op < FILE_SERVICE_OP_COUNT) {
            fs_shared_stats.op_counts[op]++;
        }
        fs_stats_end();
    }

    if (s_reqq == NULL) {
        fs_reply_ex(fs_rsp_op(msg[0]), WU_ST_EBUSY, wu_rd16(msg + 2), wu_rd32(msg + 4),
                    NULL, 0, false);
        return;
    }

    s_scratch.len = (uint32_t)len;
    memcpy(s_scratch.msg, msg, len);
    if (xQueueSend(s_reqq, &s_scratch, 0) != pdTRUE) {
        fs_stats_begin();
        fs_shared_stats.req_dropped++;
        fs_stats_end();
        ESP_LOGW(TAG, "请求队列满，立刻回 EBUSY（op=%u xid=%u）",
                 (unsigned)msg[0], (unsigned)wu_rd16(msg + 2));
        fs_reply_ex(fs_rsp_op(msg[0]), WU_ST_EBUSY, wu_rd16(msg + 2), wu_rd32(msg + 4),
                    NULL, 0, false);
    }
}

/* ============================ 服务任务 ==================================== */

static void fs_svc_task(void *arg)
{
    fs_req_t req;
    uint32_t link_gen = webusb_proto_link_generation();

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_reqq, &req, pdMS_TO_TICKS(FS_SVC_IDLE_MS)) == pdTRUE) {
            fs_dispatch(&req);
            continue;
        }
        /* 空闲时看一眼链路"代"：主机断线重连后旧句柄一定无效，关掉防泄漏
           （file.js 的 FileClient 在断链时会 abortAll，不会再来 CLOSE）。 */
        {
            uint32_t gen = webusb_proto_link_generation();
            if (gen != link_gen) {
                link_gen = gen;
                fs_close_all_handles("link generation changed");
            }
        }
    }
}

/* ============================ 生命周期 ==================================== */

esp_err_t file_service_start(void)
{
    const char *root;

    if (s_started) {
        return ESP_OK;
    }

    /* 请求队列：8 深 × 1028 B ≈ 8 KB。放 PSRAM（内部 RAM 更宝贵：TinyUSB/LVGL 都在抢），
       拿不到再退回内部堆。 */
    s_req_storage = (uint8_t *)heap_caps_malloc((size_t)FS_REQ_QDEPTH * sizeof(fs_req_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_req_storage_is_psram = (s_req_storage != NULL);
    if (s_req_storage == NULL) {
        s_req_storage = (uint8_t *)malloc((size_t)FS_REQ_QDEPTH * sizeof(fs_req_t));
    }
    if (s_req_storage == NULL) {
        ESP_LOGE(TAG, "请求队列分配失败（%u B）", (unsigned)(FS_REQ_QDEPTH * sizeof(fs_req_t)));
        return ESP_ERR_NO_MEM;
    }
    memset(s_req_storage, 0, (size_t)FS_REQ_QDEPTH * sizeof(fs_req_t));

    s_reqq = xQueueCreateStatic(FS_REQ_QDEPTH, sizeof(fs_req_t), s_req_storage, &s_req_qcb);
    if (s_reqq == NULL) {
        free(s_req_storage);
        s_req_storage = NULL;
        ESP_LOGE(TAG, "请求队列创建失败");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(fs_svc_task, "fs_svc", FS_SVC_TASK_STACK, NULL, FS_SVC_TASK_PRIO, NULL)
            != pdPASS) {
        vQueueDelete(s_reqq);
        s_reqq = NULL;
        free(s_req_storage);
        s_req_storage = NULL;
        ESP_LOGE(TAG, "FILE 服务任务创建失败（栈 %d B）", FS_SVC_TASK_STACK);
        return ESP_ERR_NO_MEM;
    }

    /* ⚠️ 必须在 usb_vendor_start()/usb_composite_start() 之前：HELLO 的 caps 里
       FILE 位由 `s_file_cb != NULL` 决定（webusb_proto.c: wu_caps()），而 HELLO 在
       收到 HELLO_ACK 之后就停发了 —— 注册晚了主机可能拿到一份没有 FILE 位的 caps。 */
    webusb_proto_set_file_handler(fs_on_file_msg, NULL);
    s_started = true;

    root = sd_storage_mount_point();
    ESP_LOGI(TAG, "FILE 服务已注册: handler=fs_on_file_msg root=%s mounted=%d",
             (root == NULL) ? "(null)" : root, (int)sd_storage_is_mounted());
    ESP_LOGI(TAG, "            句柄表=%d 个 | 请求队列=%d 深(%u B, %s) | 任务 fs_svc prio=%d stack=%d",
             FILE_SERVICE_MAX_HANDLES, FS_REQ_QDEPTH,
             (unsigned)(FS_REQ_QDEPTH * sizeof(fs_req_t)),
             s_req_storage_is_psram ? "PSRAM" : "internal", FS_SVC_TASK_PRIO,
             FS_SVC_TASK_STACK);
    ESP_LOGI(TAG, "            LIST_RSP 一页最多 %d 条（%dB/条 ≤ %dB）| READ≤%dB | WRITE≤%dB | OPEN_RSP=%dB",
             (int)FS_LIST_MAX_ENTRIES, FS_ENTRY_LEN,
             (int)(FS_PAYLOAD_MAX - WU_FILE_HDR_LEN), FS_READ_MAX, FS_WRITE_MAX, FS_OPEN_RSP_LEN);

    {
        esp_err_t perr = file_share_probe_start();
        if (perr != ESP_OK) {
            /* 探针起不来不影响 FILE 通道；只是少一条"PC 写入被 P4 看到"的证据链 */
            ESP_LOGE(TAG, "共享探针启动失败: %s —— FILE 通道仍可用", esp_err_to_name(perr));
        }
    }
    return ESP_OK;
}

void file_service_get_stats(file_service_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    fs_stats_begin();
    *out = fs_shared_stats;
    fs_stats_end();
    out->sd_mounted = sd_storage_is_mounted();
    out->started = s_started;
}
