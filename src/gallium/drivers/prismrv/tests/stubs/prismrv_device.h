/* host-test stand-in for the kernel's prismrv_device.h */
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
typedef uint32_t u32; typedef uint64_t u64; typedef uint8_t u8;
struct prismrv_device { int unused; };
struct drm_gem_object;
#include <drm/drm_gem.h>
static inline u32 prismrv_bo_gpuva(struct drm_gem_object *o) { return o->va; }
int prismrv_validate_stream(struct prismrv_device *pv, const u32 *stream,
                            unsigned long words, struct drm_gem_object **bos,
                            unsigned int num_bos);
