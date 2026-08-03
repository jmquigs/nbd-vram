/* nbd-vram.c - NBD server backed by GPU VRAM via CUDA
 *
 * Implements NBD fixed-newstyle protocol over a Unix socket.
 * No NVIDIA P2P or kernel symbols needed - uses cuMemcpyHtoDAsync/DtoHAsync.
 *
 * Compile: gcc -O2 -o nbd-vram nbd-vram.c -ldl -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <poll.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <endian.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <time.h>

/* PR_SET_IO_FLUSHER (Linux 5.6+) may be absent from older headers */
#ifndef PR_SET_IO_FLUSHER
#define PR_SET_IO_FLUSHER 57
#endif

/* -------------------------------------------------------------------------
 * CUDA driver API (dynamic load)
 * ---------------------------------------------------------------------- */

typedef int                CUresult;
typedef int                CUdevice;
typedef unsigned long long CUdeviceptr;
typedef struct CUctx_st    *CUcontext;
typedef struct CUstream_st *CUstream;

#define CUDA_SUCCESS           0
#define CU_CTX_SCHED_AUTO      0
#define CU_STREAM_NON_BLOCKING 1

typedef CUresult (*pfn_cuInit)(unsigned int);
typedef CUresult (*pfn_cuDeviceGet)(CUdevice *, int);
typedef CUresult (*pfn_cuCtxCreate)(CUcontext *, unsigned int, CUdevice);
typedef CUresult (*pfn_cuCtxDestroy)(CUcontext);
typedef CUresult (*pfn_cuCtxSetCurrent)(CUcontext);
typedef CUresult (*pfn_cuMemAlloc)(CUdeviceptr *, size_t);
typedef CUresult (*pfn_cuMemFree)(CUdeviceptr);
typedef CUresult (*pfn_cuMemcpyHtoDAsync)(CUdeviceptr, const void *, size_t, CUstream);
typedef CUresult (*pfn_cuMemcpyDtoHAsync)(void *, CUdeviceptr, size_t, CUstream);
typedef CUresult (*pfn_cuStreamCreate)(CUstream *, unsigned int);
typedef CUresult (*pfn_cuStreamDestroy)(CUstream);
typedef CUresult (*pfn_cuStreamSynchronize)(CUstream);
typedef CUresult (*pfn_cuMemAllocHost)(void **, size_t);
typedef CUresult (*pfn_cuMemFreeHost)(void *);
typedef CUresult (*pfn_cuGetErrorString)(CUresult, const char **);

static void                  *g_libcuda;
static pfn_cuInit              _cuInit;
static pfn_cuDeviceGet         _cuDeviceGet;
static pfn_cuCtxCreate         _cuCtxCreate;
static pfn_cuCtxDestroy        _cuCtxDestroy;
static pfn_cuCtxSetCurrent     _cuCtxSetCurrent;
static pfn_cuMemAlloc          _cuMemAlloc;
static pfn_cuMemFree           _cuMemFree;
static pfn_cuMemcpyHtoDAsync   _cuMemcpyHtoDAsync;
static pfn_cuMemcpyDtoHAsync   _cuMemcpyDtoHAsync;
static pfn_cuStreamCreate      _cuStreamCreate;
static pfn_cuStreamDestroy     _cuStreamDestroy;
static pfn_cuStreamSynchronize _cuStreamSynchronize;
static pfn_cuMemAllocHost      _cuMemAllocHost;
static pfn_cuMemFreeHost       _cuMemFreeHost;
static pfn_cuGetErrorString    _cuGetErrorString;

#define LOAD_SYM(h, name, pfn) do { \
    pfn = dlsym(h, name); \
    if (!pfn) { fprintf(stderr, "dlsym(%s) failed\n", name); return -1; } \
} while (0)

static int load_libcuda(void) {
    const char *paths[] = { "libcuda.so.1",
                             "/usr/lib/x86_64-linux-gnu/libcuda.so.1",
                             "/usr/lib64/libcuda.so.1", NULL };
    for (int i = 0; paths[i]; i++) {
        g_libcuda = dlopen(paths[i], RTLD_NOW);
        if (g_libcuda) { printf("[nbd-vram] loaded %s\n", paths[i]); break; }
    }
    if (!g_libcuda) { fprintf(stderr, "[nbd-vram] cannot load libcuda.so.1\n"); return -1; }
    LOAD_SYM(g_libcuda, "cuInit",                _cuInit);
    LOAD_SYM(g_libcuda, "cuDeviceGet",           _cuDeviceGet);
    LOAD_SYM(g_libcuda, "cuCtxCreate_v2",        _cuCtxCreate);
    LOAD_SYM(g_libcuda, "cuCtxDestroy_v2",       _cuCtxDestroy);
    LOAD_SYM(g_libcuda, "cuCtxSetCurrent",       _cuCtxSetCurrent);
    LOAD_SYM(g_libcuda, "cuMemAlloc_v2",         _cuMemAlloc);
    LOAD_SYM(g_libcuda, "cuMemFree_v2",          _cuMemFree);
    LOAD_SYM(g_libcuda, "cuMemcpyHtoDAsync_v2",  _cuMemcpyHtoDAsync);
    LOAD_SYM(g_libcuda, "cuMemcpyDtoHAsync_v2",  _cuMemcpyDtoHAsync);
    LOAD_SYM(g_libcuda, "cuStreamCreate",         _cuStreamCreate);
    LOAD_SYM(g_libcuda, "cuStreamDestroy_v2",     _cuStreamDestroy);
    LOAD_SYM(g_libcuda, "cuStreamSynchronize",    _cuStreamSynchronize);
    LOAD_SYM(g_libcuda, "cuMemAllocHost_v2",      _cuMemAllocHost);
    LOAD_SYM(g_libcuda, "cuMemFreeHost",          _cuMemFreeHost);
    LOAD_SYM(g_libcuda, "cuGetErrorString",       _cuGetErrorString);
    return 0;
}

static const char *cuda_err(CUresult r) {
    const char *s = NULL;
    if (_cuGetErrorString) _cuGetErrorString(r, &s);
    return s ? s : "unknown";
}

#define CUDA_CHECK(call) do { \
    CUresult _r = (call); \
    if (_r != CUDA_SUCCESS) { \
        fprintf(stderr, "[nbd-vram] " #call " failed: %s (%d)\n", cuda_err(_r), _r); \
        return -1; \
    } \
} while (0)

/* -------------------------------------------------------------------------
 * Compression backend (dynamic load)
 *
 * Swap pages compress well - the same trick zram uses - so every 4 KiB block is
 * compressed before it crosses PCIe and decompressed on the way back. zstd at
 * level 1 is the default: roughly 3x on typical anonymous pages for ~8 us per
 * page, against the ~250 us swap-fault budget this daemon already pays. lz4 is
 * the fallback (faster, weaker ratio); with neither library present the daemon
 * still runs, storing every block verbatim.
 *
 * Loaded with dlopen exactly like libcuda so the build stays
 * "gcc -O2 -Wall -o nbd-vram nbd-vram.c -ldl -lpthread" with no -dev packages.
 * ---------------------------------------------------------------------- */

/* Unit of compression: one 4 KiB block, matching PAGE_SIZE and therefore the
 * granularity the swap subsystem actually issues. CSLOT_SIZE is the host-side
 * staging slot for one block's compressed bytes; it must exceed the worst-case
 * expansion of both codecs (zstd 4176, lz4 4128 for a 4096-byte input). */
#define CBLK_SHIFT   12
#define CBLK_SIZE    (1u << CBLK_SHIFT)
#define CSLOT_SIZE   (CBLK_SIZE + 256)

#define CODEC_NONE 0
#define CODEC_ZSTD 1
#define CODEC_LZ4  2

typedef struct ZSTD_CCtx_s ZSTD_CCtx;
typedef struct ZSTD_DCtx_s ZSTD_DCtx;

typedef ZSTD_CCtx *(*pfn_ZSTD_createCCtx)(void);
typedef size_t     (*pfn_ZSTD_freeCCtx)(ZSTD_CCtx *);
typedef size_t     (*pfn_ZSTD_compressCCtx)(ZSTD_CCtx *, void *, size_t,
                                            const void *, size_t, int);
typedef ZSTD_DCtx *(*pfn_ZSTD_createDCtx)(void);
typedef size_t     (*pfn_ZSTD_freeDCtx)(ZSTD_DCtx *);
typedef size_t     (*pfn_ZSTD_decompressDCtx)(ZSTD_DCtx *, void *, size_t,
                                              const void *, size_t);
typedef unsigned   (*pfn_ZSTD_isError)(size_t);
typedef size_t     (*pfn_ZSTD_estimateCCtxSize)(int);
typedef size_t     (*pfn_ZSTD_estimateDCtxSize)(void);
typedef ZSTD_CCtx *(*pfn_ZSTD_initStaticCCtx)(void *, size_t);
typedef ZSTD_DCtx *(*pfn_ZSTD_initStaticDCtx)(void *, size_t);

typedef int (*pfn_LZ4_compress_default)(const char *, char *, int, int);
typedef int (*pfn_LZ4_decompress_safe)(const char *, char *, int, int);
typedef int (*pfn_LZ4_sizeofState)(void);
typedef int (*pfn_LZ4_compress_fast_extState)(void *, const char *, char *, int, int, int);

static void *g_libcomp;
static int   g_codec      = CODEC_NONE;
static int   g_zstd_level = 1;
static int   g_static_ctx = 0;   /* codec contexts live in our own arena */

static pfn_ZSTD_createCCtx       _ZSTD_createCCtx;
static pfn_ZSTD_freeCCtx         _ZSTD_freeCCtx;
static pfn_ZSTD_compressCCtx     _ZSTD_compressCCtx;
static pfn_ZSTD_createDCtx       _ZSTD_createDCtx;
static pfn_ZSTD_freeDCtx         _ZSTD_freeDCtx;
static pfn_ZSTD_decompressDCtx   _ZSTD_decompressDCtx;
static pfn_ZSTD_isError          _ZSTD_isError;
static pfn_ZSTD_estimateCCtxSize _ZSTD_estimateCCtxSize;
static pfn_ZSTD_estimateDCtxSize _ZSTD_estimateDCtxSize;
static pfn_ZSTD_initStaticCCtx   _ZSTD_initStaticCCtx;
static pfn_ZSTD_initStaticDCtx   _ZSTD_initStaticDCtx;

static pfn_LZ4_compress_default        _LZ4_compress_default;
static pfn_LZ4_decompress_safe         _LZ4_decompress_safe;
static pfn_LZ4_sizeofState             _LZ4_sizeofState;
static pfn_LZ4_compress_fast_extState  _LZ4_compress_fast_extState;

static const char *codec_name(int c)
{
    return c == CODEC_ZSTD ? "zstd" : c == CODEC_LZ4 ? "lz4" : "none";
}

static int try_zstd(void)
{
    void *h = dlopen("libzstd.so.1", RTLD_NOW);
    if (!h) return -1;
    _ZSTD_createCCtx     = dlsym(h, "ZSTD_createCCtx");
    _ZSTD_freeCCtx       = dlsym(h, "ZSTD_freeCCtx");
    _ZSTD_compressCCtx   = dlsym(h, "ZSTD_compressCCtx");
    _ZSTD_createDCtx     = dlsym(h, "ZSTD_createDCtx");
    _ZSTD_freeDCtx       = dlsym(h, "ZSTD_freeDCtx");
    _ZSTD_decompressDCtx = dlsym(h, "ZSTD_decompressDCtx");
    _ZSTD_isError        = dlsym(h, "ZSTD_isError");
    if (!_ZSTD_createCCtx || !_ZSTD_freeCCtx || !_ZSTD_compressCCtx ||
        !_ZSTD_createDCtx || !_ZSTD_freeDCtx || !_ZSTD_decompressDCtx ||
        !_ZSTD_isError) {
        dlclose(h);
        return -1;
    }
    /* Preferred: place the codec workspaces in memory we own, so compressing a
     * swap page provably cannot enter the allocator. Absent on very old
     * libzstd, in which case we fall back to heap contexts warmed at thread
     * start - the workspace is then allocated once, before any I/O. */
    _ZSTD_estimateCCtxSize = dlsym(h, "ZSTD_estimateCCtxSize");
    _ZSTD_estimateDCtxSize = dlsym(h, "ZSTD_estimateDCtxSize");
    _ZSTD_initStaticCCtx   = dlsym(h, "ZSTD_initStaticCCtx");
    _ZSTD_initStaticDCtx   = dlsym(h, "ZSTD_initStaticDCtx");
    g_static_ctx = _ZSTD_estimateCCtxSize && _ZSTD_estimateDCtxSize &&
                   _ZSTD_initStaticCCtx   && _ZSTD_initStaticDCtx;

    g_libcomp = h;
    g_codec   = CODEC_ZSTD;
    return 0;
}

static int try_lz4(void)
{
    void *h = dlopen("liblz4.so.1", RTLD_NOW);
    if (!h) return -1;
    _LZ4_compress_default = dlsym(h, "LZ4_compress_default");
    _LZ4_decompress_safe  = dlsym(h, "LZ4_decompress_safe");
    if (!_LZ4_compress_default || !_LZ4_decompress_safe) {
        dlclose(h);
        return -1;
    }
    /* Same reasoning as zstd: LZ4_compress_default puts a 16 KiB state on the
     * stack, which is fine, but the extState form makes it explicit. */
    _LZ4_sizeofState            = dlsym(h, "LZ4_sizeofState");
    _LZ4_compress_fast_extState = dlsym(h, "LZ4_compress_fast_extState");
    g_static_ctx = _LZ4_sizeofState && _LZ4_compress_fast_extState;

    g_libcomp = h;
    g_codec   = CODEC_LZ4;
    return 0;
}

/* VRAM_COMPRESS=zstd|lz4|none picks the codec, VRAM_COMPRESS_LEVEL the zstd
 * level. Unset means "zstd if available, else lz4, else uncompressed". */
static void load_libcompress(void)
{
    const char *want = getenv("VRAM_COMPRESS");
    const char *lvl  = getenv("VRAM_COMPRESS_LEVEL");

    if (lvl && *lvl) {
        g_zstd_level = atoi(lvl);
        if (g_zstd_level < 1)  g_zstd_level = 1;
        if (g_zstd_level > 19) g_zstd_level = 19;
    }

    if (want && (!strcmp(want, "none") || !strcmp(want, "0") || !strcmp(want, "off"))) {
        printf("[nbd-vram] compression disabled (VRAM_COMPRESS=%s)\n", want);
        return;
    }
    if (want && !strcmp(want, "lz4")) {
        if (try_lz4() == 0) { printf("[nbd-vram] compression: lz4\n"); return; }
        fprintf(stderr, "[nbd-vram] liblz4.so.1 not usable, falling back\n");
    }
    if (!want || !strcmp(want, "zstd") || !strcmp(want, "lz4")) {
        if (g_codec == CODEC_NONE && try_zstd() == 0) {
            printf("[nbd-vram] compression: zstd level %d\n", g_zstd_level);
            return;
        }
    }
    if (g_codec == CODEC_NONE && try_lz4() == 0) {
        printf("[nbd-vram] compression: lz4 (libzstd.so.1 unavailable)\n");
        return;
    }
    if (g_codec == CODEC_NONE)
        fprintf(stderr, "[nbd-vram] no compression library found (libzstd.so.1/liblz4.so.1)"
                        " - storing blocks uncompressed\n");
}

/* Per-worker codec state. Sized at startup, carved out of the worker's own
 * pre-faulted arena, so nothing on the I/O path can call malloc. */
struct comp_ctx {
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;
    void      *lz4_state;
};

static size_t comp_cws_bytes(void)
{
    if (g_codec == CODEC_ZSTD && g_static_ctx) return _ZSTD_estimateCCtxSize(g_zstd_level);
    if (g_codec == CODEC_LZ4  && g_static_ctx) return (size_t)_LZ4_sizeofState();
    return 0;
}

static size_t comp_dws_bytes(void)
{
    if (g_codec == CODEC_ZSTD && g_static_ctx) return _ZSTD_estimateDCtxSize();
    return 0;
}

static int comp_ctx_init(struct comp_ctx *c, void *cws, size_t cn, void *dws, size_t dn)
{
    memset(c, 0, sizeof(*c));
    if (g_codec == CODEC_ZSTD) {
        if (g_static_ctx) {
            c->cctx = _ZSTD_initStaticCCtx(cws, cn);
            c->dctx = _ZSTD_initStaticDCtx(dws, dn);
        } else {
            c->cctx = _ZSTD_createCCtx();
            c->dctx = _ZSTD_createDCtx();
        }
        return (c->cctx && c->dctx) ? 0 : -1;
    }
    if (g_codec == CODEC_LZ4 && g_static_ctx) c->lz4_state = cws;
    return 0;
}

static void comp_ctx_fini(struct comp_ctx *c)
{
    if (g_codec == CODEC_ZSTD && !g_static_ctx) {
        if (c->cctx) _ZSTD_freeCCtx(c->cctx);
        if (c->dctx) _ZSTD_freeDCtx(c->dctx);
    }
    memset(c, 0, sizeof(*c));
}

/* Compress one CBLK_SIZE block from src into dst (CSLOT_SIZE bytes of room).
 * Returns the number of bytes to store: less than CBLK_SIZE when the codec won,
 * exactly CBLK_SIZE when the block is stored verbatim because it did not. That
 * equivalence is why no separate "raw" flag is needed anywhere else. */
static uint32_t comp_block(struct comp_ctx *c, const void *src, void *dst)
{
    size_t n = 0;

    if (g_codec == CODEC_ZSTD) {
        size_t r = _ZSTD_compressCCtx(c->cctx, dst, CSLOT_SIZE, src, CBLK_SIZE, g_zstd_level);
        if (!_ZSTD_isError(r) && r > 0 && r < CBLK_SIZE) n = r;
    } else if (g_codec == CODEC_LZ4) {
        int r = c->lz4_state
              ? _LZ4_compress_fast_extState(c->lz4_state, src, dst, CBLK_SIZE, CSLOT_SIZE, 1)
              : _LZ4_compress_default(src, dst, CBLK_SIZE, CSLOT_SIZE);
        if (r > 0 && r < (int)CBLK_SIZE) n = (size_t)r;
    }
    if (n == 0) { memcpy(dst, src, CBLK_SIZE); n = CBLK_SIZE; }
    return (uint32_t)n;
}

static int decomp_block(struct comp_ctx *c, const void *src, uint32_t clen, void *dst)
{
    if (clen == CBLK_SIZE) { memcpy(dst, src, CBLK_SIZE); return 0; }
    if (g_codec == CODEC_ZSTD) {
        size_t r = _ZSTD_decompressDCtx(c->dctx, dst, CBLK_SIZE, src, clen);
        return (_ZSTD_isError(r) || r != CBLK_SIZE) ? -1 : 0;
    }
    if (g_codec == CODEC_LZ4) {
        int r = _LZ4_decompress_safe(src, dst, (int)clen, CBLK_SIZE);
        return (r != (int)CBLK_SIZE) ? -1 : 0;
    }
    return -1;   /* short block stored with no codec loaded: impossible */
}

/* Run once per worker before it serves anything. Proves the codec round-trips
 * on this machine and, on the fallback path, forces libzstd to allocate its
 * workspace here rather than inside a swap write. */
static int comp_selftest(struct comp_ctx *c, char *page, char *slot)
{
    char out[CBLK_SIZE];   /* per-thread: workers self-test concurrently */
    for (int p = 0; p < 3; p++) {
        for (int i = 0; i < (int)CBLK_SIZE; i++)
            page[i] = p == 0 ? 0 : p == 1 ? (char)0xAA : (char)(i * 31 + p);
        uint32_t n = comp_block(c, page, slot);
        if (decomp_block(c, slot, n, out) != 0 || memcmp(out, page, CBLK_SIZE) != 0) {
            fprintf(stderr, "[nbd-vram] compression self-test failed (%s, pattern %d)\n",
                    codec_name(g_codec), p);
            return -1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * NBD fixed-newstyle protocol constants
 * ---------------------------------------------------------------------- */

/* Handshake magic */
#define NBD_MAGIC_INIT     UINT64_C(0x4e42444d41474943)  /* "NBDMAGIC" */
#define NBD_IHAVEOPT       UINT64_C(0x49484156454f5054)  /* "IHAVEOPT" */
#define NBD_OPT_REP_MAGIC  UINT64_C(0x3e889045565a9)

/* Server handshake flags */
#define NBD_FLAG_FIXED_NEWSTYLE  0x0001
#define NBD_FLAG_NO_ZEROES       0x0002

/* Client handshake flags */
#define NBD_FLAG_C_FIXED_NEWSTYLE 0x00000001
#define NBD_FLAG_C_NO_ZEROES      0x00000002

/* Options (client→server) */
#define NBD_OPT_EXPORT_NAME  1
#define NBD_OPT_ABORT        2
#define NBD_OPT_LIST         3
#define NBD_OPT_INFO         6
#define NBD_OPT_GO           7

/* Option replies (server→client) */
#define NBD_REP_ACK          1
#define NBD_REP_SERVER       2
#define NBD_REP_INFO         3
#define NBD_REP_FLAG_ERROR   UINT32_C(0x80000000)
#define NBD_REP_ERR_UNSUP    (NBD_REP_FLAG_ERROR | 1)

/* Info types */
#define NBD_INFO_EXPORT      0

/* Transmission flags (per-export) */
#define NBD_FLAG_HAS_FLAGS      0x0001
#define NBD_FLAG_SEND_FLUSH     0x0004
#define NBD_FLAG_SEND_TRIM      0x0020
#define NBD_FLAG_CAN_MULTI_CONN 0x0100

/* Transmission request magic */
#define NBD_REQUEST_MAGIC    0x25609513
#define NBD_RESPONSE_MAGIC   0x67446698

/* Commands */
#define NBD_CMD_READ         0
#define NBD_CMD_WRITE        1
#define NBD_CMD_DISC         2
#define NBD_CMD_FLUSH        3
#define NBD_CMD_TRIM         4

/* 28-byte NBD request header, shared by the per-op and batched paths */
struct nbd_req_hdr {
    uint32_t magic;
    uint16_t flags;
    uint16_t type;
    uint64_t handle;   /* opaque, echoed back verbatim (stays network order) */
    uint64_t from;
    uint32_t len;
} __attribute__((packed));

/* -------------------------------------------------------------------------
 * I/O helpers
 * ---------------------------------------------------------------------- */

static int recv_all(int fd, void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = recv(fd, (char *)buf + done, len - done, 0);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(fd, (const char *)buf + done, len - done, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int drain(int fd, uint32_t len) {
    char buf[4096];
    while (len > 0) {
        uint32_t chunk = (len > sizeof(buf)) ? sizeof(buf) : len;
        if (recv_all(fd, buf, chunk) != 0) return -1;
        len -= chunk;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * NBD option reply helpers
 * ---------------------------------------------------------------------- */

static int send_opt_reply(int fd, uint32_t opt, uint32_t reply_type,
                           const void *data, uint32_t data_len)
{
    struct {
        uint64_t magic;
        uint32_t opt;
        uint32_t reply_type;
        uint32_t len;
    } __attribute__((packed)) hdr;

    hdr.magic      = htobe64(NBD_OPT_REP_MAGIC);
    hdr.opt        = htonl(opt);
    hdr.reply_type = htonl(reply_type);
    hdr.len        = htonl(data_len);

    if (send_all(fd, &hdr, sizeof(hdr)) != 0) return -1;
    if (data_len > 0 && send_all(fd, data, data_len) != 0) return -1;
    return 0;
}

static int send_export_info(int fd, uint32_t opt, uint64_t size, uint16_t tx_flags)
{
    struct {
        uint16_t info_type;   /* NBD_INFO_EXPORT = 0 */
        uint64_t export_size;
        uint16_t tx_flags;
    } __attribute__((packed)) info;

    info.info_type   = htons(NBD_INFO_EXPORT);
    info.export_size = htobe64(size);
    info.tx_flags    = htons(tx_flags);

    return send_opt_reply(fd, opt, NBD_REP_INFO, &info, sizeof(info));
}

/* -------------------------------------------------------------------------
 * NBD fixed-newstyle handshake
 * ---------------------------------------------------------------------- */

static int nbd_handshake(int fd, uint64_t disk_size)
{
    /* Phase 1: server greeting */
    struct {
        uint64_t magic1;
        uint64_t magic2;
        uint16_t srv_flags;
    } __attribute__((packed)) greeting;

    greeting.magic1    = htobe64(NBD_MAGIC_INIT);
    greeting.magic2    = htobe64(NBD_IHAVEOPT);
    greeting.srv_flags = htons(NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES);

    if (send_all(fd, &greeting, sizeof(greeting)) != 0) return -1;

    /* Phase 2: client flags */
    uint32_t client_flags_net;
    if (recv_all(fd, &client_flags_net, 4) != 0) return -1;
    uint32_t client_flags = ntohl(client_flags_net);
    int no_zeroes = !!(client_flags & NBD_FLAG_C_NO_ZEROES);

    /* Phase 3: option haggling */
    for (;;) {
        struct {
            uint64_t ihaveopt;
            uint32_t opt;
            uint32_t opt_len;
        } __attribute__((packed)) opt_hdr;

        if (recv_all(fd, &opt_hdr, sizeof(opt_hdr)) != 0) return -1;
        if (be64toh(opt_hdr.ihaveopt) != NBD_IHAVEOPT) return -1;

        uint32_t opt     = ntohl(opt_hdr.opt);
        uint32_t opt_len = ntohl(opt_hdr.opt_len);

        /* Limit option payload to something sane */
        if (opt_len > 65536) return -1;

        uint16_t tx_flags = NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH |
                            NBD_FLAG_SEND_TRIM | NBD_FLAG_CAN_MULTI_CONN;

        switch (opt) {
        case NBD_OPT_EXPORT_NAME:
            /* Drain the export name (we only have one export) */
            if (drain(fd, opt_len) != 0) return -1;
            /* Reply: export size + tx_flags [+ 124 zeros if needed] */
            {
                struct {
                    uint64_t size;
                    uint16_t tx_flags;
                } __attribute__((packed)) info;
                info.size     = htobe64(disk_size);
                info.tx_flags = htons(tx_flags);
                if (send_all(fd, &info, sizeof(info)) != 0) return -1;
                if (!no_zeroes) {
                    char zeros[124] = {0};
                    if (send_all(fd, zeros, sizeof(zeros)) != 0) return -1;
                }
            }
            return 0;  /* transmission begins */

        case NBD_OPT_GO:
        case NBD_OPT_INFO:
            if (drain(fd, opt_len) != 0) return -1;
            if (send_export_info(fd, opt, disk_size, tx_flags) != 0) return -1;
            if (send_opt_reply(fd, opt, NBD_REP_ACK, NULL, 0) != 0) return -1;
            if (opt == NBD_OPT_GO)
                return 0;  /* transmission begins */
            break;

        case NBD_OPT_LIST:
            /* One anonymous export */
            if (drain(fd, opt_len) != 0) return -1;
            {
                uint32_t name_len = htonl(0);
                if (send_opt_reply(fd, opt, NBD_REP_SERVER, &name_len, 4) != 0)
                    return -1;
            }
            if (send_opt_reply(fd, opt, NBD_REP_ACK, NULL, 0) != 0) return -1;
            break;

        case NBD_OPT_ABORT:
            drain(fd, opt_len);
            send_opt_reply(fd, opt, NBD_REP_ACK, NULL, 0);
            return -1;

        default:
            if (drain(fd, opt_len) != 0) return -1;
            if (send_opt_reply(fd, opt, NBD_REP_ERR_UNSUP, NULL, 0) != 0)
                return -1;
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Transmission loop
 * ---------------------------------------------------------------------- */

#define DEFAULT_SIZE_MB 7168
#define SIZE_ALIGN      (64 * 1024)
#define SOCK_PATH       "/run/nbd-vram.sock"
#define NBD_THREADS_MAX     64
#define NBD_THREADS_DEFAULT  4

/* Oversized requests (> BATCH_SLOT), FLUSH and TRIM stream through the per-op
 * path in windows of this many blocks, so that path also gets one stream sync
 * per window instead of one per block. */
#define LEGACY_WINDOW_BLOCKS 64
#define IO_BUF_SIZE          (LEGACY_WINDOW_BLOCKS * CBLK_SIZE)

/* Request-level batching: drain up to N already-queued requests, issue all their
 * VRAM copies, then ONE cuStreamSynchronize for the whole batch. Amortises both
 * the per-op socket round-trip and the per-op CUDA launch+sync (the two halves of
 * the small-IO floor). Requests larger than a slot fall back to the per-op path. */
#define BATCH_SLOT          (64 * 1024)   /* max per-request size that batches */
#define BATCH_BLOCKS        (BATCH_SLOT / CBLK_SIZE)
#define BATCH_DEPTH_DEFAULT 32
#define BATCH_DEPTH_MAX     256

/* Compression turns one request into one copy per 4 KiB block, so a batch of
 * large requests can queue far more device copies than it used to: 32 x 64 KiB
 * was 32 launches before and would be 512 now. Past this many queued copies the
 * per-launch CPU cost outweighs what the shared synchronize saves, so the batch
 * closes early. Swap I/O is 4 KiB - one copy per request - and never reaches it. */
#define MAX_BATCH_COPIES    64

static CUdeviceptr  g_vram_ptr;
static uint64_t     g_vram_size;   /* physical VRAM: the compressed backing store */
static uint64_t     g_disk_size;   /* logical size advertised to the kernel */
static CUcontext    g_cu_ctx;
static int          g_listen_fd  = -1;
static volatile int g_running    = 1;
static volatile sig_atomic_t g_term_requested = 0;
static int          g_nbd_threads = NBD_THREADS_DEFAULT;
static volatile int g_client_fds[NBD_THREADS_MAX];
static int          g_batch_enabled = 1;                  /* VRAM_BATCH=0 disables */
static int          g_batch_depth   = BATCH_DEPTH_DEFAULT; /* VRAM_BATCH_DEPTH */
static int          g_batch_debug   = 0;                  /* VRAM_BATCH_DEBUG=1 */
static unsigned long g_batch_count   = 0;                  /* flushes that coalesced (n>1) */
static unsigned long g_batch_ops     = 0;                  /* ops in those n>1 flushes */
static unsigned long g_flush_count   = 0;                  /* every batched-path flush, incl n==1 */
static unsigned long g_flush_ops     = 0;                  /* ops across all flushes (true depth) */
static unsigned long g_legacy_ops    = 0;                  /* READ/WRITE via the per-op path */
static unsigned long g_flush_copies  = 0;                  /* device copies issued by those flushes */
static volatile int  g_worker_failed = 0;                  /* a worker could not initialise */

/* Compressed-store accounting, reported once a minute by stats_worker. The two
 * that the allocator owns (g_slot_bytes, g_extents_used) are updated under
 * g_alloc_mu; the rest use __sync_fetch_and_add. All are read without a lock -
 * a stats line is allowed to be a few operations stale. */
static unsigned long g_blocks_live = 0;   /* logical blocks holding data */
static unsigned long g_codec_bytes = 0;   /* sum of stored lengths, pre-rounding */
static unsigned long g_slot_bytes  = 0;   /* sum of allocated slot sizes: real usage */
static unsigned long g_raw_blocks  = 0;   /* live blocks the codec could not shrink */
static unsigned long g_enospc      = 0;   /* writes rejected for want of VRAM */
static unsigned long g_trim_blocks = 0;   /* blocks freed by NBD_CMD_TRIM */
static unsigned long g_read_retry   = 0;  /* reads whose block was rewritten mid-DMA */
static int           g_stats_interval = 60;  /* VRAM_STATS_INTERVAL_SEC, 0 = off */

/* The stats thread reads these while workers are updating them. Every update is
 * atomic (either __sync_fetch_and_add or, for the two the allocator owns, a
 * relaxed atomic under g_alloc_mu), so this read is well-defined rather than
 * merely usually-fine. Relaxed is the right ordering: a stats line is allowed
 * to be a few operations stale, it just may not be torn. */
#define ST_READ(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)

static int clients_connected(void) {
    for (int i = 0; i < g_nbd_threads; i++)
        if (g_client_fds[i] >= 0) return 1;
    return 0;
}

static void sig_handler(int sig) {
    (void)sig;
    g_term_requested = 1;
    /* A connected client means the kernel may still have live swap pages on
     * this device. Dying now frees the VRAM behind them - the kernel reads
     * the failed page-in as hardware memory corruption and MCE-kills every
     * process that had pages swapped, PID 1 included. Do NOT exit; keep
     * serving until swapoff completes and nbd-client -d drops the
     * connection, then the workers finish the exit. */
    if (clients_connected()) {
        static const char msg[] =
            "[nbd-vram] SIGTERM with client attached - draining, exit deferred until swap detaches\n";
        ssize_t w = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)w;
        return;
    }
    g_running = 0;
    /* shutdown active client sockets so threads blocked in recv_all() unblock.
     * Threads waiting for a new connection wake on their own via the poll()
     * timeout in thread_worker - closing the listen fd here would NOT reliably
     * interrupt a thread sitting in accept(), which is what hung shutdown. */
    for (int i = 0; i < g_nbd_threads; i++) {
        int fd = g_client_fds[i];
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
    }
}

/* -------------------------------------------------------------------------
 * Block index
 *
 * Compression breaks the identity map this daemon used to have (device address
 * = g_vram_ptr + offset), because a 4 KiB logical block now occupies a variable
 * number of bytes somewhere else entirely. This table is that indirection: one
 * entry per logical block, giving the device offset and stored length.
 *
 * It is allocated once with MAP_POPULATE and then written, so every page is
 * resident before the first I/O. A page fault here would happen while servicing
 * a swap request, which is exactly the recursion mlockall/PR_SET_IO_FLUSHER
 * exist to prevent.
 * ---------------------------------------------------------------------- */

/* An entry is a single uint64_t so a reader can take a consistent snapshot with
 * one atomic load, no lock:
 *
 *   bits  0..29  grain  device offset / ALLOC_GRAIN
 *   bits 30..42  clen   stored bytes, 1..4096; 0 means the block was never written
 *   bits 43..63  gen    per-block, bumped on every publish
 *
 * A zeroed entry is therefore the natural "unwritten" state, and clen == 4096
 * means the block is stored verbatim, so no separate raw flag is needed.
 *
 * The generation is what makes lock-free reads safe. A reader snapshots the
 * entry, issues its DtoH, and re-reads the entry before decompressing: if a
 * writer replaced the block in the meantime the slot it read may since have been
 * freed and handed to some other block, so those bytes must be discarded rather
 * than decompressed and returned. Handing the wrong page to a swap-in is silent
 * memory corruption in whichever process owned it, so this check is the single
 * most safety-critical line in the file. Matching gen means the slot was never
 * displaced and the bytes are ours; a mismatch means retry. ABA would need 2^21
 * writes to one block inside a single ~10 us DMA window.
 *
 * A stale read is always harmless in itself: grain and clen come from an entry
 * that was valid when published, so the DtoH is always inside the heap. */
#define ENT_GRAIN_BITS 30
#define ENT_CLEN_BITS  13
#define ENT_GEN_MASK   0x1FFFFFu

#define IDX_LOCKS  4096          /* stripe count; must be a power of two */

static inline uint32_t ent_grain(uint64_t e) { return (uint32_t)(e & ((1u << ENT_GRAIN_BITS) - 1)); }
static inline uint32_t ent_clen (uint64_t e) { return (uint32_t)((e >> ENT_GRAIN_BITS) & ((1u << ENT_CLEN_BITS) - 1)); }
static inline uint64_t ent_make(uint32_t grain, uint32_t clen, uint32_t gen)
{
    return (uint64_t)grain
         | ((uint64_t)clen << ENT_GRAIN_BITS)
         | ((uint64_t)(gen & ENT_GEN_MASK) << (ENT_GRAIN_BITS + ENT_CLEN_BITS));
}
static inline uint32_t ent_gen(uint64_t e)
{
    return (uint32_t)(e >> (ENT_GRAIN_BITS + ENT_CLEN_BITS)) & ENT_GEN_MASK;
}

static uint64_t *g_index;
static uint64_t  g_nblocks;

/* Publishing takes a stripe lock as well as the CAS. The lock is not what makes
 * a read safe - the generation is - but it gives the read-modify-write path and
 * the reader's give-up fallback something to serialize against, so both are
 * guaranteed to terminate rather than merely likely to. */
static pthread_mutex_t g_idx_lock[IDX_LOCKS];

#define IDXL(blk) (&g_idx_lock[(blk) & (IDX_LOCKS - 1)])

static inline uint64_t idx_load(uint64_t blk)
{
    return __atomic_load_n(&g_index[blk], __ATOMIC_ACQUIRE);
}

/* -------------------------------------------------------------------------
 * Extent allocator
 *
 * Compressed blocks are variable-length, so the VRAM heap needs a real
 * allocator. This is a slab design: the heap is carved into fixed extents, each
 * handed to one size class and holding a flat array of equal-size slots tracked
 * by a bitmap. Allocation and free are both O(1) amortised and, more
 * importantly, the bookkeeping is a fixed 0.05% of the heap - about 3.6 MiB for
 * 7 GiB - all of it allocated up front. Nothing on the I/O path calls malloc.
 *
 * (The obvious alternative, per-class free stacks preallocated to their worst
 * case, needs 379 MiB for the same heap. That memory has to come out of the RAM
 * this daemon exists to conserve, so it is not an option.)
 *
 * Compaction is deliberately absent: an extent is only recycled when every slot
 * in it is free. Slots are never moved.
 * ---------------------------------------------------------------------- */

#define SLOT_NONE     0xFFFFFFFFu                    /* allocation failed / no slot held */
#define ALLOC_GRAIN   64
#define NCLASS        (CBLK_SIZE / ALLOC_GRAIN)      /* 64: class k holds (k+1)*64 B */
#define EXTENT_SHIFT  20
#define EXTENT_SIZE   (1u << EXTENT_SHIFT)           /* 1 MiB */
#define EXT_GRAINS    (EXTENT_SIZE / ALLOC_GRAIN)    /* 16384 */
#define EXT_BMWORDS   (EXT_GRAINS / 64)              /* 256 words = 2 KiB per extent */
#define EXT_NONE      0xFFFFFFFFu

/* Partial extents are kept on per-class lists bucketed by how full they are, and
 * allocation takes from the fullest non-empty bucket: a nearly-full extent gets
 * topped off and retires from the lists, while a nearly-empty one is left alone
 * so the trims still arriving for it can finish emptying it.
 *
 * Do not read more into that than it delivers. It was written to stop the
 * generational mixing that collapsed heap occupancy to 27.6% in the field, and
 * measured against a single-list build it did not move occupancy at all -
 * neither did the exact inverse policy. Which partial extent gets topped off is
 * not what decides whether extents ever drain, because a fresh extent is only
 * committed when a class has no partial at all, so every new block lands in
 * some existing extent's hole regardless. docs/heap-occupancy.md section 4.1
 * has the numbers. What the bins are actually good for is the shape they give
 * the stats line, and for finishing extents off rather than leaving a spread of
 * half-used ones.
 *
 * Bin 0 is >= 75% full, bin NBINS-1 the emptiest; a full extent is on no list at
 * all. Setting NBINS to 1 reproduces the old single-list behaviour exactly,
 * which is how that A/B was run. Cost is NCLASS * NBINS * 4 = 1 KiB, which
 * matters: allocator memory comes out of the RAM this daemon exists to
 * conserve. */
#define NBINS         4
#define BIN_FULL      NBINS                          /* on no partial list */

struct extent {
    uint32_t next, prev;   /* intrusive list: free list, or the class partial list */
    uint16_t nfree;
    uint16_t nslots;
    uint8_t  cls;
    uint8_t  used;         /* 1 once assigned to a class */
    uint16_t hint;         /* bitmap word to resume scanning from */
};

static struct extent  *g_ext;
static uint64_t       *g_extbm;      /* n_extents * EXT_BMWORDS */
static uint32_t        g_n_extents;
static uint32_t        g_extents_used;
static uint32_t        g_extents_peak;               /* high-water mark of the above */
static uint32_t        g_class_bin[NCLASS][NBINS];   /* partial lists, fullest bin first */
static uint32_t        g_bin_extents[NBINS];         /* population of each bin, for stats */
static uint32_t        g_free_head = EXT_NONE;
static pthread_mutex_t g_alloc_mu;

/* Both lists are doubly linked so an extent that empties out can leave its
 * class's partial list without a scan. An extent is on the free list, on
 * exactly one partial list, or (when full) on neither. */
static void lst_push(uint32_t *head, uint32_t e)
{
    g_ext[e].prev = EXT_NONE;
    g_ext[e].next = *head;
    if (*head != EXT_NONE) g_ext[*head].prev = e;
    *head = e;
}

static void lst_remove(uint32_t *head, uint32_t e)
{
    if (g_ext[e].prev != EXT_NONE) g_ext[g_ext[e].prev].next = g_ext[e].next;
    else                           *head = g_ext[e].next;
    if (g_ext[e].next != EXT_NONE) g_ext[g_ext[e].next].prev = g_ext[e].prev;
    g_ext[e].next = g_ext[e].prev = EXT_NONE;
}

/* Which partial list an extent belongs on, derived from its current occupancy so
 * no per-extent state has to be kept in sync. Callers holding g_alloc_mu read it
 * before mutating nfree and pass the result to ext_rebin() afterwards. */
static inline uint32_t ext_bin(const struct extent *e)
{
    if (e->nfree == 0) return BIN_FULL;
    uint32_t q = ((uint32_t)(e->nslots - e->nfree) * NBINS) / e->nslots;
    if (q >= NBINS) q = NBINS - 1;   /* unreachable while nfree > 0; keeps the
                                        index in range regardless */
    return NBINS - 1 - q;
}

static void bin_link(uint32_t ei, uint32_t bin)
{
    lst_push(&g_class_bin[g_ext[ei].cls][bin], ei);
    __atomic_fetch_add(&g_bin_extents[bin], 1, __ATOMIC_RELAXED);
}

static void bin_unlink(uint32_t ei, uint32_t bin)
{
    lst_remove(&g_class_bin[g_ext[ei].cls][bin], ei);
    __atomic_fetch_sub(&g_bin_extents[bin], 1, __ATOMIC_RELAXED);
}

/* Settle an extent onto the right list after nfree changed. `from` is the bin it
 * was on before the change, BIN_FULL if it was on none. Most allocations and
 * frees do not cross a bin boundary, so the common path is one comparison. */
static void ext_rebin(uint32_t ei, uint32_t from)
{
    uint32_t to = ext_bin(&g_ext[ei]);
    if (to == from) return;
    if (from != BIN_FULL) bin_unlink(ei, from);
    if (to   != BIN_FULL) bin_link(ei, to);
}

/* The fullest partial extent of a class, or EXT_NONE if it has none. */
static uint32_t class_pick(uint32_t cls)
{
    for (uint32_t b = 0; b < NBINS; b++)
        if (g_class_bin[cls][b] != EXT_NONE) return g_class_bin[cls][b];
    return EXT_NONE;
}

/* Hand a fresh extent to a size class. Slots that do not divide evenly into the
 * extent leave trailing bits in the bitmap; those are marked used at init so the
 * scan can never hand out a slot that runs off the end. */
static void ext_init(uint32_t ei, uint32_t cls)
{
    struct extent *e = &g_ext[ei];
    uint64_t *bm = g_extbm + (size_t)ei * EXT_BMWORDS;

    e->cls    = (uint8_t)cls;
    e->used   = 1;
    e->nslots = (uint16_t)(EXTENT_SIZE / ((cls + 1) * ALLOC_GRAIN));
    e->nfree  = e->nslots;
    e->hint   = 0;

    uint32_t full = e->nslots / 64, rem = e->nslots % 64;
    memset(bm, 0, (size_t)EXT_BMWORDS * sizeof(uint64_t));
    if (rem) bm[full] = ~0ULL << rem;
    for (uint32_t w = full + (rem ? 1 : 0); w < EXT_BMWORDS; w++) bm[w] = ~0ULL;
}

/* Claim one slot from an extent known to have a free one. Caller holds g_alloc_mu. */
static uint32_t ext_take(uint32_t ei)
{
    struct extent *e = &g_ext[ei];
    uint64_t *bm = g_extbm + (size_t)ei * EXT_BMWORDS;
    uint32_t from = ext_bin(e);

    for (uint32_t i = 0; i < EXT_BMWORDS; i++) {
        uint32_t w = e->hint + i;
        if (w >= EXT_BMWORDS) w -= EXT_BMWORDS;
        if (bm[w] == ~0ULL) continue;
        int b = __builtin_ctzll(~bm[w]);
        bm[w] |= 1ULL << b;
        e->hint = (uint16_t)w;
        e->nfree--;
        ext_rebin(ei, from);
        uint32_t slot = w * 64 + (uint32_t)b;
        return ei * EXT_GRAINS + slot * (e->cls + 1u);
    }
    return SLOT_NONE;   /* unreachable while nfree > 0 */
}

/* Reserve room for nbytes. Returns the grain offset and, via *slot_bytes, how
 * much was actually reserved (>= nbytes). SLOT_NONE means the heap is full. */
static uint32_t slot_alloc(uint32_t nbytes, uint32_t *slot_bytes)
{
    uint32_t cls = (nbytes - 1) / ALLOC_GRAIN;
    uint32_t ei, grain = SLOT_NONE;

    pthread_mutex_lock(&g_alloc_mu);

    ei = class_pick(cls);
    if (ei == EXT_NONE && g_free_head != EXT_NONE) {
        ei = g_free_head;
        lst_remove(&g_free_head, ei);
        ext_init(ei, cls);
        bin_link(ei, ext_bin(&g_ext[ei]));
        uint32_t used = __atomic_add_fetch(&g_extents_used, 1, __ATOMIC_RELAXED);
        if (used > ST_READ(g_extents_peak))
            __atomic_store_n(&g_extents_peak, used, __ATOMIC_RELAXED);
    }
    /* No extent of our own class and no fresh one left: take a slot from a
     * larger class rather than fail while the heap still has room. Wasteful,
     * but "out of memory" is much more expensive than a rounded-up slot. */
    if (ei == EXT_NONE) {
        for (uint32_t c = cls + 1; c < NCLASS; c++) {
            ei = class_pick(c);
            if (ei != EXT_NONE) break;
        }
    }
    if (ei != EXT_NONE) {
        grain = ext_take(ei);
        if (grain != SLOT_NONE) {
            *slot_bytes = (g_ext[ei].cls + 1u) * ALLOC_GRAIN;
            __atomic_fetch_add(&g_slot_bytes, *slot_bytes, __ATOMIC_RELAXED);
        }
    }

    pthread_mutex_unlock(&g_alloc_mu);
    return grain;
}

/* Release a slot. The size class comes from the owning extent, not from the
 * caller, so a slot rounded up into a larger class still returns to the right
 * place. Returns the number of bytes released. */
static uint32_t slot_free(uint32_t grain)
{
    uint32_t ei = grain / EXT_GRAINS;

    pthread_mutex_lock(&g_alloc_mu);

    struct extent *e = &g_ext[ei];
    uint32_t slot = (grain % EXT_GRAINS) / (e->cls + 1u);
    uint64_t *bm  = g_extbm + (size_t)ei * EXT_BMWORDS;
    uint32_t sz   = (e->cls + 1u) * ALLOC_GRAIN;

    uint32_t from = ext_bin(e);   /* BIN_FULL if this extent had no free slot */

    bm[slot / 64] &= ~(1ULL << (slot % 64));
    e->nfree++;
    if (slot / 64 < e->hint) e->hint = (uint16_t)(slot / 64);
    ext_rebin(ei, from);

    if (e->nfree == e->nslots) {   /* fully empty: recycle for any class */
        bin_unlink(ei, ext_bin(e));
        e->used = 0;
        lst_push(&g_free_head, ei);
        __atomic_fetch_sub(&g_extents_used, 1, __ATOMIC_RELAXED);
    }
    __atomic_fetch_sub(&g_slot_bytes, sz, __ATOMIC_RELAXED);

    pthread_mutex_unlock(&g_alloc_mu);
    return sz;
}

static uint64_t heap_committed(void)
{
    return (uint64_t)ST_READ(g_extents_used) * EXTENT_SIZE;
}

/* Index and allocator are sized from the VRAM actually obtained and the logical
 * size finally chosen, so this runs after both are known. */
static int store_init(void)
{
    g_nblocks = g_disk_size >> CBLK_SHIFT;

    /* MAP_POPULATE plus an explicit write pass: the first is a hint, the second
     * is the guarantee. Every page must be resident and writable before any I/O,
     * because faulting one while servicing a swap request is exactly the
     * recursion mlockall and PR_SET_IO_FLUSHER exist to prevent. */
    size_t idx_sz = (size_t)g_nblocks * sizeof(uint64_t);
    g_index = mmap(NULL, idx_sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (g_index == MAP_FAILED) {
        fprintf(stderr, "[nbd-vram] index mmap of %zu MiB failed: %s\n",
                idx_sz >> 20, strerror(errno));
        g_index = NULL;
        return -1;
    }
    memset(g_index, 0, idx_sz);   /* entry 0 == never written */

    for (int i = 0; i < IDX_LOCKS; i++) pthread_mutex_init(&g_idx_lock[i], NULL);
    pthread_mutex_init(&g_alloc_mu, NULL);

    g_n_extents = (uint32_t)(g_vram_size / EXTENT_SIZE);
    if (g_n_extents == 0) {
        fprintf(stderr, "[nbd-vram] VRAM allocation smaller than one %u MiB extent\n",
                EXTENT_SIZE >> 20);
        return -1;
    }
    g_ext   = calloc(g_n_extents, sizeof(struct extent));
    g_extbm = calloc((size_t)g_n_extents * EXT_BMWORDS, sizeof(uint64_t));
    if (!g_ext || !g_extbm) {
        fprintf(stderr, "[nbd-vram] extent bookkeeping allocation failed\n");
        return -1;
    }
    for (uint32_t c = 0; c < NCLASS; c++)
        for (uint32_t b = 0; b < NBINS; b++) g_class_bin[c][b] = EXT_NONE;
    for (uint32_t b = 0; b < NBINS; b++) g_bin_extents[b] = 0;
    g_free_head = EXT_NONE;
    for (uint32_t i = g_n_extents; i-- > 0; ) {   /* build the list in ascending order */
        g_ext[i].next = g_ext[i].prev = EXT_NONE;
        lst_push(&g_free_head, i);
    }

    size_t alloc_sz = (size_t)g_n_extents * sizeof(struct extent) +
                      (size_t)g_n_extents * EXT_BMWORDS * sizeof(uint64_t);
    printf("[nbd-vram] store: %llu blocks of %u B in %u extents of %u MiB "
           "(host cost: index %.1f MiB, allocator %.1f MiB)\n",
           (unsigned long long)g_nblocks, CBLK_SIZE, g_n_extents, EXTENT_SIZE >> 20,
           (double)idx_sz / (1024.0 * 1024.0), (double)alloc_sz / (1024.0 * 1024.0));
    return 0;
}

static void store_fini(void)
{
    if (g_index) munmap(g_index, (size_t)g_nblocks * sizeof(uint64_t));
    free(g_ext);
    free(g_extbm);
    g_index = NULL;
    g_ext   = NULL;
    g_extbm = NULL;
}

/* One NBD response header */
struct nbd_resp_hdr {
    uint32_t magic;
    uint32_t error;
    uint64_t handle;
} __attribute__((packed));

/* One batched op: a READ or WRITE that fits in a slot of the batch buffer */
struct bop {
    uint64_t handle;   /* network order, echoed verbatim */
    uint64_t offset;
    uint32_t len;
    uint16_t cmd;
    uint32_t error;
    char    *slot;     /* uncompressed host staging for this request */
    uint32_t op0, nops;   /* the blocks this request expands into */
};

/* One 4 KiB block transfer: the unit everything below actually moves. */
struct blkop {
    uint64_t blk;
    uint64_t ent;      /* read: the snapshot to validate against after the DMA */
    uint32_t grain;
    uint32_t clen;
    uint32_t req;      /* owning struct bop, for error attribution */
    uint8_t  cmd;
    uint8_t  err;      /* 0, ENOSPC or EIO */
    uint8_t  dma;      /* this op has a copy queued on the stream */
    char    *cslot;    /* pinned staging for the compressed bytes */
    char    *page;     /* the uncompressed 4 KiB */
};

/* Everything one connection needs. Replaces the loose buffers that used to be
 * threaded through handle_client/handle_one/flush_batch as separate arguments. */
struct worker {
    int             idx;
    CUstream        stream;
    struct comp_ctx cc;
    void           *cws, *dws;      /* codec workspaces */
    size_t          cws_n, dws_n;
    char           *cstage;         /* pinned: compressed DMA staging, CSLOT_SIZE stride */
    char           *ubuf;           /* uncompressed batch staging / send buffer */
    char           *iobuf;          /* uncompressed staging for the oversized path */
    char           *scratch;        /* one uncompressed page, for RMW and partial reads */
    char           *cscratch;       /* pinned; compressed staging for the single-block paths.
                                     * Separate from cstage because those paths can run
                                     * while a batch's cstage slots are still live, and
                                     * separate from scratch because decompressing a
                                     * buffer into itself does not work. */
    struct blkop   *ops;
    struct bop     *reqs;
    uint32_t        maxops;
};

/* Overflow-safe bounds check against the LOGICAL device. Written as
 * offset > size || length > size - offset so that a near-2^64 offset cannot wrap
 * the sum and slip past, which the naive offset + length > size would allow. */
static inline int oob(uint64_t offset, uint32_t length) {
    return offset > g_disk_size || (uint64_t)length > g_disk_size - offset;
}

static inline CUdeviceptr dev_at(uint32_t grain)
{
    return g_vram_ptr + (uint64_t)grain * ALLOC_GRAIN;
}

/* Rate-limited so a full device cannot flood the journal - the kernel is already
 * printing its own "Write-error on swap-device" line per failed page. */
static void note_enospc(void)
{
    static time_t last;
    time_t now = time(NULL);
    __sync_fetch_and_add(&g_enospc, 1);
    if (now != last) {
        last = now;
        fprintf(stderr, "[nbd-vram] VRAM heap full (%llu/%llu MiB committed) - "
                        "returning ENOSPC; lower VRAM_DISK_SIZE_MB or free swap\n",
                (unsigned long long)(heap_committed() >> 20),
                (unsigned long long)(g_vram_size >> 20));
    }
}

/* Read a full 28-byte request header WITHOUT blocking if none is queued. The
 * first byte(s) decide: nothing queued -> 0 (adaptive batch cutoff); a header
 * began arriving -> finish it blocking to stay frame-aligned. */
static int recv_hdr_nb(int fd, struct nbd_req_hdr *h)
{
    ssize_t r = recv(fd, h, sizeof(*h), MSG_DONTWAIT);
    if (r == 0) return -1;                                   /* peer closed */
    if (r < 0)  return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    if ((size_t)r == sizeof(*h)) return 1;
    return (recv_all(fd, (char *)h + r, sizeof(*h) - r) == 0) ? 1 : -1;
}

/* -------------------------------------------------------------------------
 * Block primitives
 *
 * Every data path below is built from these three phases, so that the single
 * cuStreamSynchronize per batch survives compression:
 *
 *   A  prepare - compress and allocate (write), or snapshot the index (read)
 *   B  issue every copy on the stream, then ONE synchronize
 *   C  publish (write), or validate and decompress (read)
 *
 * Phase C for a write MUST complete before that request's reply is sent. A
 * client that has been acked is entitled to read the data back over any other
 * connection, and NBD_FLAG_CAN_MULTI_CONN promises exactly that.
 * ---------------------------------------------------------------------- */

/* Phase A, write: compress into the op's staging slot and reserve VRAM for it.
 * Deliberately does NOT look at the current index entry - the block being
 * replaced is read at publish time instead, so that two writes racing on the
 * same block each free exactly the slot they displaced. Reading it here would
 * have both of them see, and then both free, the same slot. */
static void wr_prepare(struct worker *w, struct blkop *bo)
{
    uint32_t slot_bytes = 0;

    bo->clen  = comp_block(&w->cc, bo->page, bo->cslot);
    bo->grain = slot_alloc(bo->clen, &slot_bytes);
    if (bo->grain == SLOT_NONE) {
        bo->err = ENOSPC;
        bo->dma = 0;
        note_enospc();
        return;
    }
    bo->dma = 1;
}

/* Phase C, write: swap the new entry in and release whatever it displaced.
 *
 * Every mutation of an entry - here, in rmw_block and in trim_range - happens
 * under that block's stripe lock, so the load and the store are one atomic
 * step with respect to other writers and each writer frees exactly the slot it
 * displaced. Two writes racing on one block therefore leak nothing and free
 * nothing twice, which reading the displaced entry back in phase A instead
 * would get wrong: both would see, and both would free, the same slot. */
static void wr_publish(struct blkop *bo)
{
    pthread_mutex_t *mu = IDXL(bo->blk);
    uint64_t old, nv;

    pthread_mutex_lock(mu);
    old = idx_load(bo->blk);
    nv  = ent_make(bo->grain, bo->clen, ent_gen(old) + 1);
    __atomic_store_n(&g_index[bo->blk], nv, __ATOMIC_RELEASE);
    pthread_mutex_unlock(mu);

    if (ent_clen(old)) {
        slot_free(ent_grain(old));
        __sync_fetch_and_sub(&g_codec_bytes, ent_clen(old));
        if (ent_clen(old) == CBLK_SIZE) __sync_fetch_and_sub(&g_raw_blocks, 1);
    } else {
        __sync_fetch_and_add(&g_blocks_live, 1);
    }
    __sync_fetch_and_add(&g_codec_bytes, bo->clen);
    if (bo->clen == CBLK_SIZE) __sync_fetch_and_add(&g_raw_blocks, 1);
}

/* A prepared write that will never be published (its request failed elsewhere)
 * still holds a reservation. */
static void wr_abort(struct blkop *bo)
{
    if (bo->grain != SLOT_NONE) slot_free(bo->grain);
    bo->grain = SLOT_NONE;
    bo->dma   = 0;
}

/* Phase A, read. clen == 0 is an unwritten block: no DMA, reads back as zeros
 * the way a fresh block device is expected to. */
static void rd_prepare(struct blkop *bo)
{
    bo->ent   = idx_load(bo->blk);
    bo->clen  = ent_clen(bo->ent);
    bo->grain = ent_grain(bo->ent);
    bo->dma   = bo->clen != 0;
}

/* Phase C, read. Returns 0 on success, 1 if the block was rewritten under us and
 * the caller should retry, -1 if the stored bytes did not decompress. */
static int rd_finish(struct worker *w, struct blkop *bo)
{
    if (!bo->clen) { memset(bo->page, 0, CBLK_SIZE); return 0; }

    /* The validation described at the index definition: if the entry changed,
     * our slot may already have been freed and reused by another block, so these
     * bytes are not ours. Checked BEFORE decompressing, so a decompress failure
     * below is an unambiguous corruption signal rather than a lost race. */
    if (idx_load(bo->blk) != bo->ent) return 1;

    if (decomp_block(&w->cc, bo->cslot, bo->clen, bo->page) != 0) {
        fprintf(stderr, "[nbd-vram] decompress failed for block %llu (%u bytes) - "
                        "VRAM corruption\n",
                (unsigned long long)bo->blk, bo->clen);
        return -1;
    }
    return 0;
}

/* Last-resort single-block read, serialized against publishers so it cannot be
 * raced. Only reached when a block loses the validation above repeatedly, which
 * needs a writer hitting the same block on every attempt. */
static int rd_block_locked(struct worker *w, uint64_t blk, char *page)
{
    pthread_mutex_t *mu = IDXL(blk);
    uint64_t ent;
    int rc = 0;

    pthread_mutex_lock(mu);
    ent = idx_load(blk);
    if (!ent_clen(ent)) {
        memset(page, 0, CBLK_SIZE);
    } else {
        CUresult r = _cuMemcpyDtoHAsync(w->cscratch, dev_at(ent_grain(ent)),
                                        ent_clen(ent), w->stream);
        if (r != CUDA_SUCCESS) {
            fprintf(stderr, "[nbd-vram] DtoH failed: %s\n", cuda_err(r));
            rc = -1;
        } else {
            _cuStreamSynchronize(w->stream);
            rc = decomp_block(&w->cc, w->cscratch, ent_clen(ent), page) == 0 ? 0 : -1;
        }
    }
    pthread_mutex_unlock(mu);
    return rc;
}

/* Phase B for a prepared set of ops, plus phase C. Splitting them would not buy
 * anything: the whole point is that the ops share one synchronize.
 * Per-op outcomes land in bo->err; the return value is only about whether the
 * connection can continue. */
static int do_blocks(struct worker *w, struct blkop *ops, uint32_t n)
{
    int queued = 0;

    for (uint32_t i = 0; i < n; i++) {
        struct blkop *bo = &ops[i];
        if (!bo->dma || bo->err) continue;
        CUresult r = (bo->cmd == NBD_CMD_READ)
            ? _cuMemcpyDtoHAsync(bo->cslot, dev_at(bo->grain), bo->clen, w->stream)
            : _cuMemcpyHtoDAsync(dev_at(bo->grain), bo->cslot, bo->clen, w->stream);
        if (r != CUDA_SUCCESS) {
            fprintf(stderr, "[nbd-vram] %s failed: %s\n",
                    bo->cmd == NBD_CMD_READ ? "DtoH" : "HtoD", cuda_err(r));
            bo->err = EIO;
            bo->dma = 0;
            continue;
        }
        queued++;
    }
    if (queued) _cuStreamSynchronize(w->stream);   /* one sync for the whole set */

    /* Reads first: validate, decompress, and collect anything that lost its
     * race for a retry round. */
    for (int round = 0; round < 4; round++) {
        uint32_t again = 0;
        for (uint32_t i = 0; i < n; i++) {
            struct blkop *bo = &ops[i];
            if (bo->cmd != NBD_CMD_READ || bo->err || bo->dma == 2) continue;
            int rc = rd_finish(w, bo);
            if (rc == 0)      { bo->dma = 2; }          /* 2 = done */
            else if (rc < 0)  { bo->err = EIO; }
            else              { again++; }
        }
        if (!again) break;
        __sync_fetch_and_add(&g_read_retry, again);

        /* Re-snapshot the losers and give them their own sync. */
        int q = 0;
        for (uint32_t i = 0; i < n; i++) {
            struct blkop *bo = &ops[i];
            if (bo->cmd != NBD_CMD_READ || bo->err || bo->dma == 2) continue;
            rd_prepare(bo);
            if (!bo->dma) continue;
            if (_cuMemcpyDtoHAsync(bo->cslot, dev_at(bo->grain), bo->clen,
                                   w->stream) != CUDA_SUCCESS) {
                bo->err = EIO;
                bo->dma = 0;
                continue;
            }
            q++;
        }
        if (q) _cuStreamSynchronize(w->stream);
    }
    /* Anything still contested after four rounds gets the locked path, which
     * cannot lose. This is what makes the retry loop terminate rather than
     * merely usually terminate. */
    for (uint32_t i = 0; i < n; i++) {
        struct blkop *bo = &ops[i];
        if (bo->cmd != NBD_CMD_READ || bo->err || bo->dma == 2) continue;
        if (rd_block_locked(w, bo->blk, bo->page) != 0) bo->err = EIO;
        bo->dma = 2;
    }

    /* grain == SLOT_NONE means this write never got a reservation, either
     * because the heap was full or because a sibling block of the same request
     * failed and wr_abort gave it back. Publishing it would point the block at
     * nothing. */
    for (uint32_t i = 0; i < n; i++) {
        struct blkop *bo = &ops[i];
        if (bo->cmd != NBD_CMD_WRITE || bo->err || bo->grain == SLOT_NONE) continue;
        wr_publish(bo);
    }
    return 0;
}

/* Fill in the fixed parts of a block op. */
static void blkop_set(struct worker *w, struct blkop *bo, uint32_t i,
                      uint64_t blk, uint8_t cmd, char *page, uint32_t req)
{
    bo->blk   = blk;
    bo->cmd   = cmd;
    bo->page  = page;
    bo->req   = req;
    bo->err   = 0;
    bo->dma   = 0;
    bo->ent   = 0;
    bo->clen  = 0;
    bo->grain = SLOT_NONE;
    bo->cslot = w->cstage + (size_t)i * CSLOT_SIZE;
}

/* -------------------------------------------------------------------------
 * Byte-range access (arbitrary offset and length)
 * ---------------------------------------------------------------------- */

/* Whole-block reads for an aligned run, in windows of maxops. */
static int load_blocks(struct worker *w, uint64_t blk, char *dst, uint32_t nblocks)
{
    while (nblocks) {
        uint32_t n = nblocks > w->maxops ? w->maxops : nblocks;
        for (uint32_t i = 0; i < n; i++)
            blkop_set(w, &w->ops[i], i, blk + i, NBD_CMD_READ, dst + (size_t)i * CBLK_SIZE, 0);
        for (uint32_t i = 0; i < n; i++) rd_prepare(&w->ops[i]);
        do_blocks(w, w->ops, n);
        for (uint32_t i = 0; i < n; i++) if (w->ops[i].err) return EIO;
        blk     += n;
        dst     += (size_t)n * CBLK_SIZE;
        nblocks -= n;
    }
    return 0;
}

static int store_blocks(struct worker *w, uint64_t blk, const char *src, uint32_t nblocks)
{
    int err = 0;
    while (nblocks) {
        uint32_t n = nblocks > w->maxops ? w->maxops : nblocks;
        for (uint32_t i = 0; i < n; i++)
            blkop_set(w, &w->ops[i], i, blk + i, NBD_CMD_WRITE,
                      (char *)src + (size_t)i * CBLK_SIZE, 0);
        for (uint32_t i = 0; i < n; i++) wr_prepare(w, &w->ops[i]);
        do_blocks(w, w->ops, n);
        for (uint32_t i = 0; i < n; i++)
            if (w->ops[i].err && (!err || w->ops[i].err == EIO)) err = w->ops[i].err;
        blk     += n;
        src     += (size_t)n * CBLK_SIZE;
        nblocks -= n;
    }
    return err;
}

/* Partial-block write. Held under the block's stripe lock start to finish, so
 * two writes to disjoint halves of one block cannot lose each other - the block
 * layer considers those non-overlapping and expects both to survive.
 * Swap never issues sub-page I/O; mkswap, blkid and dd can. */
static int rmw_block(struct worker *w, uint64_t blk, uint32_t boff,
                     const char *src, uint32_t cnt)
{
    pthread_mutex_t *mu = IDXL(blk);
    uint64_t old;
    uint32_t clen, slot_bytes, grain;
    int err = 0;

    pthread_mutex_lock(mu);

    old = idx_load(blk);
    if (!ent_clen(old)) {
        memset(w->scratch, 0, CBLK_SIZE);
    } else {
        CUresult r = _cuMemcpyDtoHAsync(w->cscratch, dev_at(ent_grain(old)),
                                        ent_clen(old), w->stream);
        if (r != CUDA_SUCCESS) { err = EIO; goto out; }
        _cuStreamSynchronize(w->stream);
        if (decomp_block(&w->cc, w->cscratch, ent_clen(old), w->scratch) != 0) {
            fprintf(stderr, "[nbd-vram] decompress failed for block %llu during RMW\n",
                    (unsigned long long)blk);
            err = EIO;
            goto out;
        }
    }

    memcpy(w->scratch + boff, src, cnt);

    clen  = comp_block(&w->cc, w->scratch, w->cscratch);
    grain = slot_alloc(clen, &slot_bytes);
    if (grain == SLOT_NONE) { note_enospc(); err = ENOSPC; goto out; }

    if (_cuMemcpyHtoDAsync(dev_at(grain), w->cscratch, clen, w->stream) != CUDA_SUCCESS) {
        slot_free(grain);
        err = EIO;
        goto out;
    }
    _cuStreamSynchronize(w->stream);

    __atomic_store_n(&g_index[blk], ent_make(grain, clen, ent_gen(old) + 1),
                     __ATOMIC_RELEASE);
    if (ent_clen(old)) {
        slot_free(ent_grain(old));
        __sync_fetch_and_sub(&g_codec_bytes, ent_clen(old));
        if (ent_clen(old) == CBLK_SIZE) __sync_fetch_and_sub(&g_raw_blocks, 1);
    } else {
        __sync_fetch_and_add(&g_blocks_live, 1);
    }
    __sync_fetch_and_add(&g_codec_bytes, clen);
    if (clen == CBLK_SIZE) __sync_fetch_and_add(&g_raw_blocks, 1);

out:
    pthread_mutex_unlock(mu);
    return err;
}

/* Read an arbitrary byte range. Head and tail partial blocks are decompressed
 * whole and sliced; the aligned middle goes through the batched path. */
static int load_range(struct worker *w, uint64_t off, char *dst, uint32_t n)
{
    if (off & (CBLK_SIZE - 1)) {
        uint32_t boff = off & (CBLK_SIZE - 1);
        uint32_t cnt  = CBLK_SIZE - boff;
        if (cnt > n) cnt = n;
        if (load_blocks(w, off >> CBLK_SHIFT, w->scratch, 1) != 0) return EIO;
        memcpy(dst, w->scratch + boff, cnt);
        dst += cnt; off += cnt; n -= cnt;
    }
    if (n >= CBLK_SIZE) {
        uint32_t nb = n >> CBLK_SHIFT;
        if (load_blocks(w, off >> CBLK_SHIFT, dst, nb) != 0) return EIO;
        dst += (size_t)nb * CBLK_SIZE;
        off += (uint64_t)nb * CBLK_SIZE;
        n   -= nb * CBLK_SIZE;
    }
    if (n) {
        if (load_blocks(w, off >> CBLK_SHIFT, w->scratch, 1) != 0) return EIO;
        memcpy(dst, w->scratch, n);
    }
    return 0;
}

static int store_range(struct worker *w, uint64_t off, const char *src, uint32_t n)
{
    int err = 0, e;

    if (off & (CBLK_SIZE - 1)) {
        uint32_t boff = off & (CBLK_SIZE - 1);
        uint32_t cnt  = CBLK_SIZE - boff;
        if (cnt > n) cnt = n;
        if ((e = rmw_block(w, off >> CBLK_SHIFT, boff, src, cnt)) != 0) err = e;
        src += cnt; off += cnt; n -= cnt;
    }
    if (n >= CBLK_SIZE) {
        uint32_t nb = n >> CBLK_SHIFT;
        if ((e = store_blocks(w, off >> CBLK_SHIFT, src, nb)) != 0 &&
            (!err || e == EIO)) err = e;
        src += (size_t)nb * CBLK_SIZE;
        off += (uint64_t)nb * CBLK_SIZE;
        n   -= nb * CBLK_SIZE;
    }
    if (n) {
        if ((e = rmw_block(w, off >> CBLK_SHIFT, 0, src, n)) != 0 &&
            (!err || e == EIO)) err = e;
    }
    return err;
}

/* NBD_CMD_TRIM: give the VRAM back. With an overcommitted logical size this is
 * the main thing keeping the heap from filling, because the kernel returns
 * freed swap slots here as discards.
 *
 * swapon issues one discard across the whole device, so this has to stay cheap
 * over millions of untouched blocks: the unlocked pre-check makes that a plain
 * scan of the index. Racing a concurrent write to the same block is fine -
 * discard is advisory and overlapping requests are unordered anyway.
 * Only wholly-covered blocks are freed; partial ends are left alone. */
static void trim_range(uint64_t off, uint64_t len)
{
    uint64_t b0 = (off + CBLK_SIZE - 1) >> CBLK_SHIFT;
    uint64_t b1 = (off + len) >> CBLK_SHIFT;

    if (b1 > g_nblocks) b1 = g_nblocks;
    for (uint64_t b = b0; b < b1; b++) {
        if (!ent_clen(idx_load(b))) continue;

        pthread_mutex_t *mu = IDXL(b);
        pthread_mutex_lock(mu);
        uint64_t old = idx_load(b);
        if (ent_clen(old))
            __atomic_store_n(&g_index[b], ent_make(0, 0, ent_gen(old) + 1),
                             __ATOMIC_RELEASE);
        pthread_mutex_unlock(mu);

        if (ent_clen(old)) {
            slot_free(ent_grain(old));
            __sync_fetch_and_sub(&g_codec_bytes, ent_clen(old));
            if (ent_clen(old) == CBLK_SIZE) __sync_fetch_and_sub(&g_raw_blocks, 1);
            __sync_fetch_and_sub(&g_blocks_live, 1);
            __sync_fetch_and_add(&g_trim_blocks, 1);
        }
    }
}

/* -------------------------------------------------------------------------
 * Request handling
 * ---------------------------------------------------------------------- */

/* Per-request path: oversized requests, misaligned ones, FLUSH/TRIM, and
 * VRAM_BATCH=0. Streams through the block layer in IO_BUF_SIZE windows so this
 * path also gets one stream synchronize per window rather than one per block.
 * Returns 0 to keep serving, -1 to drop the connection. */
static int handle_one(int fd, const struct nbd_req_hdr *h, struct worker *w)
{
    uint16_t cmd    = ntohs(h->type);
    uint64_t handle = h->handle;
    uint64_t offset = be64toh(h->from);
    uint32_t length = ntohl(h->len);
    uint32_t error  = 0;

    if (g_batch_debug && (cmd == NBD_CMD_READ || cmd == NBD_CMD_WRITE))
        __sync_fetch_and_add(&g_legacy_ops, 1);

    if ((cmd == NBD_CMD_READ || cmd == NBD_CMD_WRITE || cmd == NBD_CMD_TRIM) &&
        oob(offset, length)) {
        fprintf(stderr, "[nbd-vram] oob off=%llu len=%u\n",
                (unsigned long long)offset, length);
        error = EINVAL;
    }

    if (cmd == NBD_CMD_WRITE) {
        uint32_t remaining = length;
        uint64_t voff      = offset;
        while (remaining > 0) {
            uint32_t chunk = remaining > IO_BUF_SIZE ? IO_BUF_SIZE : remaining;
            /* End every non-final chunk on a block boundary, so a misaligned
             * request pays for read-modify-write only at its real edges rather
             * than at every window boundary. */
            if (chunk < remaining) {
                uint64_t end = (voff + chunk) & ~(uint64_t)(CBLK_SIZE - 1);
                if (end > voff) chunk = (uint32_t)(end - voff);
            }
            if (recv_all(fd, w->iobuf, chunk) != 0) return -1;
            if (!error) {
                int e = store_range(w, voff, w->iobuf, chunk);
                if (e && (!error || e == EIO)) error = e;
            }
            voff      += chunk;
            remaining -= chunk;
        }
    } else if (cmd == NBD_CMD_FLUSH) {
        /* A write is not acked until its index entry is published, so there is
         * no write-back state to flush. Kept advertised because the kernel
         * expects a swap device to support it. */
        _cuStreamSynchronize(w->stream);
    } else if (cmd == NBD_CMD_TRIM) {
        if (!error) trim_range(offset, length);
    }

    struct nbd_resp_hdr resp;
    resp.magic  = htonl(NBD_RESPONSE_MAGIC);
    resp.error  = htonl(error);
    resp.handle = handle;
    if (send_all(fd, &resp, sizeof(resp)) != 0) return -1;
    if (error == EIO) return -1;   /* real copy failure: hard-reset the connection */
    if (error)        return 0;    /* bad request or heap full: reported, keep serving */

    if (cmd == NBD_CMD_READ) {
        uint32_t remaining = length;
        uint64_t voff      = offset;
        while (remaining > 0) {
            uint32_t chunk = remaining > IO_BUF_SIZE ? IO_BUF_SIZE : remaining;
            if (chunk < remaining) {
                uint64_t end = (voff + chunk) & ~(uint64_t)(CBLK_SIZE - 1);
                if (end > voff) chunk = (uint32_t)(end - voff);
            }
            if (load_range(w, voff, w->iobuf, chunk) != 0) return -1;
            if (send_all(fd, w->iobuf, chunk) != 0) return -1;
            remaining -= chunk;
            voff      += chunk;
        }
    }
    return 0;
}

/* Validate a header and, if it is a batchable READ/WRITE, populate *req and its
 * block ops (reading the WRITE payload now, since it must be drained in frame
 * order). Returns 1 = batched, 0 = not batchable (caller falls back to
 * handle_one), -1 = protocol/socket error.
 *
 * Misaligned requests are pushed to handle_one on purpose: they need
 * read-modify-write, which cannot be deferred to a shared synchronize. */
static int batch_admit(int fd, const struct nbd_req_hdr *hh, struct worker *w,
                       uint32_t ri, uint32_t *nops)
{
    if (ntohl(hh->magic) != NBD_REQUEST_MAGIC) {
        fprintf(stderr, "[nbd-vram] bad request magic 0x%x\n", ntohl(hh->magic));
        return -1;
    }
    uint16_t cmd = ntohs(hh->type);
    uint32_t len = ntohl(hh->len);
    uint64_t off = be64toh(hh->from);

    if ((cmd != NBD_CMD_READ && cmd != NBD_CMD_WRITE) || len == 0 || len > BATCH_SLOT)
        return 0;   /* DISC/FLUSH/TRIM or oversized: not for the batch path */
    if ((off | len) & (CBLK_SIZE - 1))
        return 0;   /* misaligned: needs the read-modify-write path */

    struct bop *req = &w->reqs[ri];
    req->handle = hh->handle;
    req->offset = off;
    req->len    = len;
    req->cmd    = cmd;
    req->error  = oob(off, len) ? EINVAL : 0;
    req->slot   = w->ubuf + (size_t)ri * BATCH_SLOT;
    req->op0    = *nops;
    req->nops   = req->error ? 0 : len >> CBLK_SHIFT;

    for (uint32_t i = 0; i < req->nops; i++) {
        uint32_t oi = *nops + i;
        blkop_set(w, &w->ops[oi], oi, (off >> CBLK_SHIFT) + i, (uint8_t)cmd,
                  req->slot + (size_t)i * CBLK_SIZE, ri);
    }
    *nops += req->nops;

    if (cmd == NBD_CMD_WRITE) {
        /* Drain the payload even when OOB, to stay frame-aligned. */
        if (recv_all(fd, req->slot, len) != 0) return -1;
    }
    return 1;
}

/* Run the prepared batch and reply to every request in it.
 *
 * NBD matches replies by handle, so a per-op error never strands the others: a
 * bad request (EINVAL) or a full heap (ENOSPC) is reported and we keep serving;
 * a real copy failure (EIO) is reported too, then the connection is dropped
 * after every reply is sent.
 *
 * Ordering within a batch is no longer the stream's FIFO property but the phase
 * structure: two writes to one block are resolved by the publish CAS, and any
 * write is published before its reply goes out. */
static int flush_batch(int fd, struct worker *w, uint32_t nreq, uint32_t nops)
{
    int hard_err = 0;

    for (uint32_t i = 0; i < nops; i++) {
        struct blkop *bo = &w->ops[i];
        if (bo->cmd == NBD_CMD_WRITE) wr_prepare(w, bo);
        else                          rd_prepare(bo);
    }

    /* A request that failed on one of its blocks must not half-apply the rest,
     * and any reservation those blocks took has to go back. */
    for (uint32_t i = 0; i < nops; i++)
        if (w->ops[i].err) w->reqs[w->ops[i].req].error = w->ops[i].err;
    for (uint32_t i = 0; i < nops; i++) {
        struct blkop *bo = &w->ops[i];
        if (bo->cmd == NBD_CMD_WRITE && !bo->err && w->reqs[bo->req].error)
            wr_abort(bo);
    }

    do_blocks(w, w->ops, nops);

    for (uint32_t i = 0; i < nops; i++) {
        struct blkop *bo = &w->ops[i];
        struct bop   *rq = &w->reqs[bo->req];
        if (bo->err && (!rq->error || bo->err == EIO)) rq->error = bo->err;
    }

    if (g_batch_debug) {
        __sync_fetch_and_add(&g_flush_count, 1);
        __sync_fetch_and_add(&g_flush_ops, nreq);
        __sync_fetch_and_add(&g_flush_copies, nops);
        if (nreq > 1) { __sync_fetch_and_add(&g_batch_count, 1);
                        __sync_fetch_and_add(&g_batch_ops, nreq); }
    }

    for (uint32_t i = 0; i < nreq; i++) {
        struct nbd_resp_hdr resp;
        resp.magic  = htonl(NBD_RESPONSE_MAGIC);
        resp.error  = htonl(w->reqs[i].error);
        resp.handle = w->reqs[i].handle;
        if (w->reqs[i].error == EIO) hard_err = 1;
        if (send_all(fd, &resp, sizeof(resp)) != 0) return -1;
        if (w->reqs[i].cmd == NBD_CMD_READ && !w->reqs[i].error) {
            if (send_all(fd, w->reqs[i].slot, w->reqs[i].len) != 0) return -1;
        }
    }
    return hard_err ? -1 : 0;   /* EIO: every reply sent, now reset the connection */
}

static int handle_client(int fd, struct worker *w)
{
    if (nbd_handshake(fd, g_disk_size) != 0) {
        fprintf(stderr, "[nbd-vram] handshake failed\n");
        return -1;
    }
    printf("[nbd-vram] handshake OK, entering transmission mode\n");

    int depth = g_batch_depth;

    while (g_running) {
        /* Block here for the first request: this is the worker's idle wait. */
        struct nbd_req_hdr h;
        if (recv_all(fd, &h, sizeof(h)) != 0) return -1;
        if (ntohl(h.magic) != NBD_REQUEST_MAGIC) {
            fprintf(stderr, "[nbd-vram] bad request magic 0x%x\n", ntohl(h.magic));
            return -1;
        }
        if (ntohs(h.type) == NBD_CMD_DISC) break;

        if (!g_batch_enabled) {
            if (handle_one(fd, &h, w) != 0) return -1;
            continue;
        }

        uint32_t nreq = 0, nops = 0;
        int adm = batch_admit(fd, &h, w, 0, &nops);
        if (adm < 0) return -1;
        if (adm == 0) {                       /* not batchable: per-request path */
            if (handle_one(fd, &h, w) != 0) return -1;
            continue;
        }
        nreq = 1;

        /* Drain whatever else is already queued, without blocking. Idle clients
         * yield a batch of 1 (= unbatched behaviour); a busy client supplies
         * depth. The adaptivity is free: we never wait to assemble a batch.
         *
         * The cap is on COPIES, not requests. One request is one copy only when
         * it is a single block; a 64 KiB request is 16 scattered slots and
         * therefore 16 launches, and letting depth requests of that size into
         * one batch would spend more CPU on launches than the shared
         * synchronize saves. */
        struct nbd_req_hdr extra;
        int trailer = 0;   /* 0 none, 1 non-batchable header in `extra`, 2 DISC */
        uint32_t copy_cap = w->maxops - BATCH_BLOCKS;
        if (copy_cap > MAX_BATCH_COPIES) copy_cap = MAX_BATCH_COPIES;

        while ((int)nreq < depth && nops < copy_cap) {
            int g = recv_hdr_nb(fd, &extra);
            if (g < 0) return -1;
            if (g == 0) break;                /* nothing more queued right now */
            if (ntohl(extra.magic) != NBD_REQUEST_MAGIC) {
                fprintf(stderr, "[nbd-vram] bad request magic 0x%x\n", ntohl(extra.magic));
                return -1;
            }
            if (ntohs(extra.type) == NBD_CMD_DISC) { trailer = 2; break; }
            int a = batch_admit(fd, &extra, w, nreq, &nops);
            if (a < 0) return -1;
            if (a == 0) { trailer = 1; break; }   /* flush, then handle it per-op */
            nreq++;
        }

        if (flush_batch(fd, w, nreq, nops) != 0) return -1;

        if (trailer == 1) { if (handle_one(fd, &extra, w) != 0) return -1; }
        else if (trailer == 2) break;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Thread worker - one per NBD connection slot
 * ---------------------------------------------------------------------- */

/* Host memory that must never fault while an I/O is in flight. Pinned memory is
 * only needed for the DMA staging buffer; everything else just has to be
 * resident, which mlockall already guarantees - the memset is what forces the
 * pages in now rather than during a swap write. */
static void *worker_alloc(size_t n, int pinned)
{
    void *p = NULL;
    if (pinned) {
        if (_cuMemAllocHost(&p, n) != CUDA_SUCCESS) return NULL;
    } else {
        p = malloc(n);
        if (!p) return NULL;
    }
    memset(p, 0, n);
    return p;
}

static int worker_init(struct worker *w, int idx)
{
    memset(w, 0, sizeof(*w));
    w->idx    = idx;
    w->maxops = (uint32_t)g_batch_depth * BATCH_BLOCKS;
    if (w->maxops < LEGACY_WINDOW_BLOCKS) w->maxops = LEGACY_WINDOW_BLOCKS;

    _cuCtxSetCurrent(g_cu_ctx);
    if (_cuStreamCreate(&w->stream, CU_STREAM_NON_BLOCKING) != CUDA_SUCCESS) {
        fprintf(stderr, "[nbd-vram] worker %d: stream creation failed\n", idx);
        return -1;
    }

    w->cws_n = comp_cws_bytes();
    w->dws_n = comp_dws_bytes();
    if (w->cws_n) w->cws = worker_alloc(w->cws_n, 0);
    if (w->dws_n) w->dws = worker_alloc(w->dws_n, 0);
    if ((w->cws_n && !w->cws) || (w->dws_n && !w->dws)) {
        fprintf(stderr, "[nbd-vram] worker %d: codec workspace allocation failed\n", idx);
        return -1;
    }
    if (comp_ctx_init(&w->cc, w->cws, w->cws_n, w->dws, w->dws_n) != 0) {
        fprintf(stderr, "[nbd-vram] worker %d: codec context init failed\n", idx);
        return -1;
    }

    w->cstage  = worker_alloc((size_t)w->maxops * CSLOT_SIZE, 1);
    w->ubuf    = worker_alloc((size_t)g_batch_depth * BATCH_SLOT, 0);
    w->iobuf   = worker_alloc(IO_BUF_SIZE, 0);
    w->scratch  = worker_alloc(CBLK_SIZE, 0);
    w->cscratch = worker_alloc(CSLOT_SIZE, 1);
    w->ops     = worker_alloc((size_t)w->maxops * sizeof(struct blkop), 0);
    w->reqs    = worker_alloc((size_t)g_batch_depth * sizeof(struct bop), 0);
    if (!w->cstage || !w->ubuf || !w->iobuf || !w->scratch || !w->cscratch ||
        !w->ops || !w->reqs) {
        fprintf(stderr, "[nbd-vram] worker %d: buffer allocation failed\n", idx);
        return -1;
    }

    /* Proves the codec round-trips here, and on the non-static fallback forces
     * libzstd to allocate its workspace now instead of inside a swap write. */
    if (comp_selftest(&w->cc, w->scratch, w->cscratch) != 0) return -1;
    memset(w->scratch, 0, CBLK_SIZE);
    return 0;
}

static void worker_fini(struct worker *w)
{
    comp_ctx_fini(&w->cc);
    if (w->cstage)   _cuMemFreeHost(w->cstage);
    if (w->cscratch) _cuMemFreeHost(w->cscratch);
    free(w->cws);
    free(w->dws);
    free(w->ubuf);
    free(w->iobuf);
    free(w->scratch);
    free(w->ops);
    free(w->reqs);
    if (w->stream) _cuStreamDestroy(w->stream);
}

static void *thread_worker(void *arg)
{
    int idx = (int)(intptr_t)arg;
    struct worker wk;
    /* PF_MEMALLOC_NOIO/PF_LOCAL_THROTTLE are per-task; pthread inheritance of
     * PR_SET_IO_FLUSHER is undocumented, so set it per worker too (cheap, the
     * failure case is already logged once from main). */
    prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);

    if (worker_init(&wk, idx) != 0) {
        worker_fini(&wk);
        g_worker_failed = 1;
        return NULL;
    }

    while (g_running) {
        /* Draining (SIGTERM arrived with a client attached): take no new
         * connections, just wait for the remaining clients to detach. The
         * thread that sees the last one go flips g_running for everyone. */
        if (g_term_requested) {
            if (!clients_connected()) {
                g_running = 0;
                printf("[nbd-vram] drain complete - last client gone, exiting\n");
                break;
            }
            struct timespec ts = { 0, 100 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            continue;
        }
        /* Wait for a connection with a 1s timeout so the loop re-checks
         * g_running and exits promptly on SIGTERM (a bare blocking accept()
         * cannot be woken by the signal handler, which hung shutdown for 90s). */
        struct pollfd pfd = { .fd = g_listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0) continue;            /* timeout or EINTR -> re-check g_running */
        int cfd = accept(g_listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        g_client_fds[idx] = cfd;
        printf("[nbd-vram] client connected\n");
        handle_client(cfd, &wk);
        g_client_fds[idx] = -1;
        close(cfd);
        printf("[nbd-vram] client disconnected\n");
    }

    worker_fini(&wk);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Stats
 * ---------------------------------------------------------------------- */

static const char *hsize(uint64_t b, char *buf, size_t n)
{
    if (b >= (1ULL << 30)) snprintf(buf, n, "%.2f GiB", (double)b / (double)(1ULL << 30));
    else if (b >= (1ULL << 20)) snprintf(buf, n, "%.1f MiB", (double)b / (double)(1ULL << 20));
    else snprintf(buf, n, "%llu KiB", (unsigned long long)(b >> 10));
    return buf;
}

/* Occupancy: how much of the committed heap actually holds slots. This is the
 * number that separates "full of data" from "full of holes", and the one the
 * fragmentation warning keys off. */
static double heap_occupancy(void)
{
    uint64_t commit = heap_committed();
    return commit ? 100.0 * (double)ST_READ(g_slot_bytes) / (double)commit : 0.0;
}

/* The line the feature exists to produce: how much the kernel thinks it has
 * stored, how much VRAM that actually costs, and the ratio between them.
 *
 * The headline ratio is stored/committed, not stored/slots: committed VRAM is
 * what runs out, and the two diverge by exactly the fragmentation. The codec
 * ratio is kept alongside it because watching them separate is how the
 * fragmentation becomes visible at all. */
static void stats_log(const char *tag)
{
    uint64_t blocks = ST_READ(g_blocks_live);
    uint64_t stored = blocks * CBLK_SIZE;
    uint64_t codec  = ST_READ(g_codec_bytes);
    uint64_t slots  = ST_READ(g_slot_bytes);
    uint64_t commit = heap_committed();
    char a[32], b[32], c[32], d[32];

    printf("[nbd-vram] %s: stored %s -> %s VRAM (%.2fx) | codec %.2fx | occupancy %.1f%% "
           "| extents %u/%u (peak %u) | heap %s/%s committed (%.1f%%) "
           "| slack %.1f%% | raw %.1f%% | enospc %lu trimmed %lu retries %lu\n",
           tag,
           hsize(stored, a, sizeof(a)),
           hsize(commit, b, sizeof(b)),
           commit ? (double)stored / (double)commit : 0.0,
           codec  ? (double)stored / (double)codec  : 0.0,
           heap_occupancy(),
           ST_READ(g_extents_used), g_n_extents, ST_READ(g_extents_peak),
           hsize(commit, c, sizeof(c)),
           hsize(g_vram_size, d, sizeof(d)),
           g_vram_size ? 100.0 * (double)commit / (double)g_vram_size : 0.0,
           slots  ? 100.0 * (double)(slots - codec) / (double)slots : 0.0,
           blocks ? 100.0 * (double)ST_READ(g_raw_blocks) / (double)blocks : 0.0,
           ST_READ(g_enospc), ST_READ(g_trim_blocks), ST_READ(g_read_retry));
    fflush(stdout);
}

/* Second line: where the partial extents actually sit. A healthy heap keeps its
 * partials in the fullest bin and few of them; a long tail in the emptiest bin
 * with a large `full` count is fragmentation accumulating. `classes` is the
 * count of size classes holding at least one partial extent - holes are
 * class-specific, so a high number means the free space is scattered across
 * classes that cannot lend to each other downwards. */
static void bins_log(const char *tag)
{
    uint32_t used    = ST_READ(g_extents_used);
    uint32_t partial = 0, classes = 0;
    char buf[160];
    int n = 0;

    for (uint32_t b = 0; b < NBINS; b++) {
        uint32_t lo = (NBINS - 1 - b) * 100 / NBINS;
        uint32_t cnt = ST_READ(g_bin_extents[b]);
        int w = snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%u-%u%% %u",
                         b ? " | " : "", lo, lo + 100 / NBINS, cnt);
        partial += cnt;
        if (w < 0 || (size_t)w >= sizeof(buf) - (size_t)n) break;
        n += w;
    }

    for (uint32_t c = 0; c < NCLASS; c++)
        for (uint32_t b = 0; b < NBINS; b++)
            if (ST_READ(g_class_bin[c][b]) != EXT_NONE) { classes++; break; }

    /* The counters are read without the lock and can disagree by an allocation
     * or two, so the subtraction is clamped rather than trusted. */
    printf("[nbd-vram] %s bins: %u used = %u full + %u partial | free %u | "
           "classes %u | partial by fullness: %s\n",
           tag, used, used > partial ? used - partial : 0, partial,
           g_n_extents > used ? g_n_extents - used : 0, classes, buf);
    fflush(stdout);
}

/* Ticks once a second so shutdown is prompt, but only reports on the interval. */
static void *stats_worker(void *arg)
{
    int tick = 0;
    int warned = 0;
    int frag_warned = 0;
    (void)arg;

    while (g_running) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);

        /* Warn on the way up to full, so the journal shows the cliff coming
         * rather than only the ENOSPC that follows it. */
        int pct = g_vram_size ? (int)(100 * heap_committed() / g_vram_size) : 0;
        int level = pct >= 99 ? 3 : pct >= 95 ? 2 : pct >= 90 ? 1 : 0;
        if (level > warned) {
            fprintf(stderr, "[nbd-vram] VRAM heap %d%% committed - writes will start "
                            "failing with ENOSPC when it fills\n", pct);
            warned = level;
        } else if (level < warned && pct < 85) {
            warned = level;   /* hysteresis, so it can warn again after recovery */
        }

        /* Fullness alone cannot tell "full of data" from "full of holes", and
         * the second one is the failure mode that surprises people: the heap
         * reaches ENOSPC while most of the committed VRAM is empty. */
        int occ = (int)heap_occupancy();
        if (!frag_warned && pct >= 75 && occ < 50) {
            fprintf(stderr, "[nbd-vram] VRAM heap %d%% committed but only %d%% occupied - "
                            "fragmentation, not data; committed VRAM cannot recede until "
                            "extents empty completely\n", pct, occ);
            frag_warned = 1;
        } else if (frag_warned && (pct < 65 || occ >= 60)) {
            frag_warned = 0;  /* hysteresis, matching the fullness warning above */
        }

        if (++tick < g_stats_interval) continue;
        tick = 0;
        stats_log("stats");
        bins_log("stats");
    }
    stats_log("final");
    bins_log("final");
    return NULL;
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(void)
{
    CUdevice  cu_dev;
    int       ret    = 1;

    /* Socket path is fixed in production; VRAM_SOCK_PATH overrides it for
     * non-root testing. Resolved early so every cleanup path sees it. */
    const char *sock_path = getenv("VRAM_SOCK_PATH");
    if (!sock_path || !*sock_path) sock_path = SOCK_PATH;

    /* Under journald stdout is a pipe and therefore fully buffered, so log lines
     * can sit in libc's buffer indefinitely. Line-buffer it so everything the
     * daemon prints - the per-minute stats especially - reaches the journal when
     * it happens. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Prevent kernel from paging out our own pages - doing so would route
     * the page fault back through this daemon, deadlocking under swap pressure.
     * Requires LimitMEMLOCK=infinity in the systemd service. VRAM_NO_MLOCK=1
     * skips it for unprivileged dev runs (MCL_FUTURE otherwise makes CUDA's huge
     * VA reservation exceed a normal user's memlock limit and cuInit OOMs). */
    if (getenv("VRAM_NO_MLOCK")) {
        fprintf(stderr, "[nbd-vram] VRAM_NO_MLOCK set - skipping mlockall (dev/test only)\n");
    } else if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        fprintf(stderr, "[nbd-vram] mlockall failed (%s) - daemon pages may be swapped, risking deadlock\n",
                strerror(errno));
    }

    /* Mark the daemon as part of the I/O flush path. Sets PF_MEMALLOC_NOIO +
     * PF_LOCAL_THROTTLE so allocations made while servicing a swap write do not
     * recurse back into reclaim/writeback - that recursion is the swap-over-NBD
     * deadlock at zero free RAM. Per-process; set before threads spawn. */
    if (prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0) != 0)
        fprintf(stderr, "[nbd-vram] PR_SET_IO_FLUSHER failed (%s) - deadlock risk under swap pressure\n",
                strerror(errno));

    /* Parsed before anything uses them: the listen backlog and the per-worker
     * buffer sizing both depend on these, and both used to run before the
     * parse and therefore always saw the defaults. */
    {
        const char *tenv = getenv("VRAM_NBD_THREADS");
        if (tenv) {
            g_nbd_threads = atoi(tenv);
            if (g_nbd_threads < 1) g_nbd_threads = 1;
            if (g_nbd_threads > NBD_THREADS_MAX) g_nbd_threads = NBD_THREADS_MAX;
        }
        const char *benv = getenv("VRAM_BATCH");
        if (benv) g_batch_enabled = atoi(benv) != 0;
        const char *bdenv = getenv("VRAM_BATCH_DEPTH");
        if (bdenv) {
            g_batch_depth = atoi(bdenv);
            if (g_batch_depth < 1) g_batch_depth = 1;
            if (g_batch_depth > BATCH_DEPTH_MAX) g_batch_depth = BATCH_DEPTH_MAX;
        }
        const char *bgenv = getenv("VRAM_BATCH_DEBUG");
        if (bgenv) g_batch_debug = atoi(bgenv) != 0;
        const char *senv = getenv("VRAM_STATS_INTERVAL_SEC");
        if (senv) {
            g_stats_interval = atoi(senv);
            if (g_stats_interval < 0) g_stats_interval = 0;
        }
    }

    if (load_libcuda() != 0) goto out;
    load_libcompress();

    for (int i = 0; i < 10; i++) {
        CUresult r = _cuInit(0);
        if (r == CUDA_SUCCESS) break;
        if (i == 9) {
            fprintf(stderr, "[nbd-vram] cuInit failed: %s\n", cuda_err(r));
            goto out;
        }
        fprintf(stderr, "[nbd-vram] cuInit attempt %d failed, retrying\n", i + 1);
        sleep(2);
    }

    if (_cuDeviceGet(&cu_dev, 0) != CUDA_SUCCESS) goto out;
    if (_cuCtxCreate(&g_cu_ctx, CU_CTX_SCHED_AUTO, cu_dev) != CUDA_SUCCESS) goto out;

    const char *env = getenv("VRAM_SETUP_SIZE_MB");
    size_t mb = env ? (size_t)atol(env) : DEFAULT_SIZE_MB;

    /* Back off 512 MiB at a time if the GPU is short on memory (e.g. display compositor loaded) */
    g_vram_ptr = 0;
    while (mb >= 1024) {
        g_vram_size = (mb * 1024ULL * 1024ULL / SIZE_ALIGN) * SIZE_ALIGN;
        printf("[nbd-vram] allocating %llu MiB of VRAM\n",
               (unsigned long long)(g_vram_size >> 20));
        CUresult alloc_r = _cuMemAlloc(&g_vram_ptr, g_vram_size);
        if (alloc_r == CUDA_SUCCESS) break;
        fprintf(stderr, "[nbd-vram] %llu MiB failed (%s), backing off 512 MiB\n",
                (unsigned long long)mb, cuda_err(alloc_r));
        g_vram_ptr = 0;
        mb -= 512;
    }
    if (!g_vram_ptr) {
        fprintf(stderr, "[nbd-vram] all allocation attempts failed\n");
        goto out_cuda;
    }
    printf("[nbd-vram] VRAM at CUDA VA 0x%llx\n", (unsigned long long)g_vram_ptr);

    if (g_vram_size / ALLOC_GRAIN >= (1ULL << ENT_GRAIN_BITS)) {
        fprintf(stderr, "[nbd-vram] %llu MiB exceeds the %llu GiB this build can index\n",
                (unsigned long long)(g_vram_size >> 20),
                (unsigned long long)(((1ULL << ENT_GRAIN_BITS) * ALLOC_GRAIN) >> 30));
        goto out_cuda;
    }

    /* The logical size the kernel sees. Compression is what makes it larger than
     * the VRAM behind it: VRAM_DISK_SIZE_MB is the explicit setting, and with it
     * unset we assume a conservative 2x, well under the ~2.5-3x that zstd
     * typically manages on anonymous pages. Overshoot is not fatal - the device
     * simply starts refusing writes with ENOSPC once the heap is full. */
    {
        const char *denv = getenv("VRAM_DISK_SIZE_MB");
        if (denv && *denv) {
            g_disk_size = (uint64_t)atol(denv) * 1024ULL * 1024ULL;
        } else {
            g_disk_size = g_vram_size * 2;
            printf("[nbd-vram] VRAM_DISK_SIZE_MB unset, defaulting to 2x VRAM\n");
        }
        if (g_codec == CODEC_NONE && g_disk_size > g_vram_size) {
            fprintf(stderr, "[nbd-vram] no codec available - capping the device at the "
                            "%llu MiB of VRAM behind it\n",
                    (unsigned long long)(g_vram_size >> 20));
            g_disk_size = g_vram_size;
        }
        g_disk_size = (g_disk_size >> CBLK_SHIFT) << CBLK_SHIFT;
        if (g_disk_size < CBLK_SIZE) {
            fprintf(stderr, "[nbd-vram] VRAM_DISK_SIZE_MB too small\n");
            goto out_cuda;
        }
        double over = (double)g_disk_size / (double)g_vram_size;
        printf("[nbd-vram] device %llu MiB logical over %llu MiB VRAM (%.2fx), codec %s\n",
               (unsigned long long)(g_disk_size >> 20),
               (unsigned long long)(g_vram_size >> 20), over, codec_name(g_codec));
        if (over > 4.0)
            fprintf(stderr, "[nbd-vram] %.2fx needs a compression ratio few workloads reach - "
                            "expect ENOSPC; watch the per-minute stats line\n", over);
    }

    if (store_init() != 0) goto out_cuda;

    /* Create Unix socket (path resolved at top of main; VRAM_SOCK_PATH may
     * override it for non-root testing). */
    unlink(sock_path);
    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { perror("socket"); goto out_cuda; }

    {
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
        if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
            { perror("bind"); goto out_cuda; }
    }
    chmod(sock_path, 0600);
    if (listen(g_listen_fd, g_nbd_threads + 1) < 0) { perror("listen"); goto out_cuda; }
    /* non-blocking so the poll()+accept() in each worker never blocks if another
     * worker grabbed the pending connection first */
    fcntl(g_listen_fd, F_SETFL, fcntl(g_listen_fd, F_GETFL, 0) | O_NONBLOCK);

    printf("[nbd-vram] listening on %s (%d threads)\n", sock_path, g_nbd_threads);

    /* sd_notify READY=1 */
    {
        const char *ns = getenv("NOTIFY_SOCKET");
        if (ns) {
            int nfd = socket(AF_UNIX, SOCK_DGRAM, 0);
            if (nfd >= 0) {
                struct sockaddr_un na = { .sun_family = AF_UNIX };
                const char *p = (ns[0] == '@') ? ns + 1 : ns;
                strncpy(na.sun_path, p, sizeof(na.sun_path) - 1);
                const char *msg = "READY=1\n";
                sendto(nfd, msg, strlen(msg), 0, (struct sockaddr *)&na, sizeof(na));
                close(nfd);
            }
        }
    }

    {
        printf("[nbd-vram] request batching %s (depth %d, slot %d KiB)\n",
               g_batch_enabled ? "on" : "off", g_batch_depth, BATCH_SLOT / 1024);

        pthread_t threads[NBD_THREADS_MAX];
        pthread_t stats_th;
        int stats_on = g_stats_interval > 0;

        for (int i = 0; i < g_nbd_threads; i++) g_client_fds[i] = -1;
        for (int i = 0; i < g_nbd_threads; i++)
            pthread_create(&threads[i], NULL, thread_worker, (void *)(intptr_t)i);
        if (stats_on && pthread_create(&stats_th, NULL, stats_worker, NULL) != 0)
            stats_on = 0;

        for (int i = 0; i < g_nbd_threads; i++)
            pthread_join(threads[i], NULL);
        g_running = 0;
        if (stats_on) pthread_join(stats_th, NULL);
    }
    ret = g_worker_failed ? 1 : 0;

out_cuda:
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    unlink(sock_path);
    store_fini();
    if (g_vram_ptr) _cuMemFree(g_vram_ptr);
    if (g_cu_ctx)   _cuCtxDestroy(g_cu_ctx);
    if (g_libcuda)  dlclose(g_libcuda);
    if (g_libcomp)  dlclose(g_libcomp);
out:
    if (g_batch_debug) {
        unsigned long batched_path = g_flush_ops;
        unsigned long total = batched_path + g_legacy_ops;
        printf("[nbd-vram] batched-path: %lu flushes, %lu ops, true avg depth %.2f\n",
               g_flush_count, g_flush_ops,
               g_flush_count ? (double)g_flush_ops / (double)g_flush_count : 0.0);
        printf("[nbd-vram]   of those, %lu coalesced (n>1) carrying %lu ops (avg %.1f)\n",
               g_batch_count, g_batch_ops,
               g_batch_count ? (double)g_batch_ops / (double)g_batch_count : 0.0);
        printf("[nbd-vram]   legacy/oversized ops: %lu  (%.1f%% of %lu total R/W requests)\n",
               g_legacy_ops, total ? 100.0 * (double)g_legacy_ops / (double)total : 0.0, total);
        /* Copies per flush is the number that matters now: one request is one
         * copy only when it is a single block, so this is what says whether the
         * shared synchronize is still paying for itself. */
        printf("[nbd-vram]   device copies: %lu (avg %.2f per flush, %.2f per op)\n",
               g_flush_copies,
               g_flush_count ? (double)g_flush_copies / (double)g_flush_count : 0.0,
               g_flush_ops   ? (double)g_flush_copies / (double)g_flush_ops   : 0.0);
    }
    printf("[nbd-vram] exiting (ret=%d)\n", ret);
    return ret;
}
