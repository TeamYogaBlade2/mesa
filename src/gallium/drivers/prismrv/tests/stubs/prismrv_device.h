/* host-test stand-in for the kernel's prismrv_device.h */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
typedef uint32_t u32; typedef uint64_t u64; typedef uint8_t u8; typedef int32_t s32;
#define ALIGN(x, a) (((x) + (a) - 1) & ~((size_t)(a) - 1))
#define lower_32_bits(n) ((u32)(n))
#define upper_32_bits(n) ((u32)(((n) >> 16) >> 16))
#define U32_MAX UINT32_MAX
struct prismrv_device { int unused; };
struct drm_gem_object;
#include <drm/drm_gem.h>
static inline u32 prismrv_bo_gpuva(struct drm_gem_object *o) { return o->va; }
typedef int (*prismrv_read_bo_fn)(void *ctx, struct drm_gem_object *bo,
                                  u64 off, void *dst, size_t len);
int prismrv_validate_stream(struct prismrv_device *pv, const u32 *stream,
                            size_t words, struct drm_gem_object **bos,
                            unsigned int num_bos);
int prismrv_validate_ta(const u32 *ta, size_t words);
bool prismrv_stream_next(const u32 *stream, size_t words, size_t *pos,
                         u32 *op, u32 *n, const u32 **payload);
size_t prismrv_stream_snapshot_size(const u32 *stream, size_t words);
int prismrv_stream_snapshot_fill(const u32 *stream, size_t words,
                                 struct drm_gem_object **bos,
                                 unsigned int num_bos, u32 snap_va, void *out,
                                 prismrv_read_bo_fn read_bo, void *ctx);
