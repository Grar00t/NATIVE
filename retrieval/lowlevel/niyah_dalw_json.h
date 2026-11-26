#ifndef NIYAH_DALW_JSON_H
#define NIYAH_DALW_JSON_H

#include <stddef.h>

#include "niyah_dalw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One JSON object: {"query","exact","count","hits":[{rank,document_id,score,shard,doc,receipt}]}.
   document_id is a 16-digit hex STRING (a uint64 does not fit a JSON number safely).
   Pass out=NULL and cap=0 to measure the result.
   Returns bytes written (without the NUL), or -1 on error / buffer too small;
   *status (may be NULL) receives a NiyahDalwStatus. k is capped at 1000. */
int niyah_dalw_search_json(const NiyahDalw *handle, const char *query, int exact,
                           size_t k, char *out, size_t cap, int *status);
/* any != 0 matches one or more query terms. */
int niyah_dalw_search_json_mode(const NiyahDalw *handle, const char *query, int exact, int any,
                           size_t k, char *out, size_t cap, int *status);

#ifdef __cplusplus
}
#endif

#endif
