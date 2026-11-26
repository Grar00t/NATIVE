#include "niyah/casper.h"

NiyahStatus niyah_casper_mmap_load(const char *path, NiyahModel *out_model)
{
    return niyah_model_mmap_load(path, out_model);
}
