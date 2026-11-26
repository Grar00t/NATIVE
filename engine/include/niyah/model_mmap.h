#ifndef NIYAH_MODEL_MMAP_H
#define NIYAH_MODEL_MMAP_H

#include "niyah/niyah.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Map a NIYAH v0x0005 raw model read-only without copying
 * or reordering tensors. Supply a zero-initialized model and release the
 * read-only mapping with niyah_model_destroy after inference. Windows accepts UTF-8 paths.
 */
NiyahStatus niyah_model_mmap_load(const char *path, NiyahModel *out_model);

#ifdef __cplusplus
}
#endif
#endif
