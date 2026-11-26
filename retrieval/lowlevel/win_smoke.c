#include <stdio.h>
#include <string.h>

#include "dalw.h"
#include "niyah_dalw.h"
#include "niyah_dalw_json.h"

int main(void)
{
    static const char *x = "weather today in Jeddah";
    const char *paths[1] = { "smoke.dalw" };
    char out[16384];
    int st = -99, n;
    DalwBuilder *b;
    NiyahDalw *h;

    remove(paths[0]);
    b = dalw_builder_new();
    if (!b || dalw_builder_add(b, "d1", x, strlen(x)) != 0 || dalw_builder_write(b, paths[0]) != 0) {
        puts("build failed");
        return 1;
    }
    dalw_builder_free(b);
    h = niyah_dalw_open(paths, 1, 1, &st);
    if (!h) {
        printf("open failed st=%d\n", st);
        return 1;
    }
    n = niyah_dalw_search_json(h, "weather", 1, 10u, out, sizeof out, &st);
    printf("n=%d st=%d\n%s\n", n, st, out);
    niyah_dalw_close(h);
    remove(paths[0]);
    return n > 0 ? 0 : 1;
}
