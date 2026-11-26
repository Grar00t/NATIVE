#ifndef NIYAH_CASPER_H
#define NIYAH_CASPER_H

#include "niyah/model_mmap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Compatibility entry point for applications built against NATIVE's first release. */
NiyahStatus niyah_casper_mmap_load(const char *path, NiyahModel *out_model);

#ifdef __cplusplus
}
#endif
#endif
