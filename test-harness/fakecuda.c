/* Stub libcuda.so.1: backs "VRAM" with host memory so nbd-vram's storage layer
 * can be exercised on a machine with no NVIDIA GPU. Test scaffolding only. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef int CUresult;
typedef unsigned long long CUdeviceptr;

int cuInit(unsigned int f) { (void)f; return 0; }
int cuDeviceGet(int *d, int o) { (void)o; *d = 0; return 0; }
int cuCtxCreate_v2(void **c, unsigned f, int d) { (void)f; (void)d; *c = (void *)0x1; return 0; }
int cuCtxDestroy_v2(void *c) { (void)c; return 0; }
int cuCtxSetCurrent(void *c) { (void)c; return 0; }

int cuMemAlloc_v2(CUdeviceptr *p, size_t n)
{
    void *m = calloc(1, n);
    if (!m) return 2;             /* CUDA_ERROR_OUT_OF_MEMORY */
    *p = (CUdeviceptr)(uintptr_t)m;
    return 0;
}
int cuMemFree_v2(CUdeviceptr p) { free((void *)(uintptr_t)p); return 0; }

int cuMemcpyHtoDAsync_v2(CUdeviceptr d, const void *s, size_t n, void *st)
{ (void)st; memcpy((void *)(uintptr_t)d, s, n); return 0; }
int cuMemcpyDtoHAsync_v2(void *d, CUdeviceptr s, size_t n, void *st)
{ (void)st; memcpy(d, (const void *)(uintptr_t)s, n); return 0; }

int cuStreamCreate(void **s, unsigned f) { (void)f; *s = (void *)0x2; return 0; }
int cuStreamDestroy_v2(void *s) { (void)s; return 0; }
int cuStreamSynchronize(void *s) { (void)s; return 0; }

int cuMemAllocHost_v2(void **p, size_t n) { *p = calloc(1, n); return *p ? 0 : 2; }
int cuMemFreeHost(void *p) { free(p); return 0; }

int cuGetErrorString(CUresult r, const char **s) { (void)r; *s = "fake"; return 0; }
