#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dalw.h"
#include "niyah_dalw.h"
#include "niyah_dalw_json.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    static const char *x0 = "local search index in Riyadh";
    static const char *x1 = "weather today in Jeddah";
    const char *path = "./niyah_dalw_json.dalw";
    const char *paths[1];
    char out[16384];
    char tiny[32];
    char title[20001];
    memset(title, 'x', sizeof title - 1u);
    title[sizeof title - 1u] = '\0';
    int st = -99, n;
    NiyahDalw *h;
    DalwBuilder *b;

    remove(path);
    b = dalw_builder_new();
    CHECK(b != NULL);
    if (!b) return 1;
    CHECK(dalw_builder_add(b, "d0", x0, strlen(x0)) == 0);
    CHECK(dalw_builder_add(b, "d1", x1, strlen(x1)) == 0);
    CHECK(dalw_builder_add(b, title, "longtitle", 9u) == 0);
    CHECK(dalw_builder_write(b, path) == 0);
    dalw_builder_free(b);

    paths[0] = path;
    h = niyah_dalw_open(paths, 1, 1, &st);
    CHECK(h != NULL && st == NIYAH_DALW_OK);
    if (!h) return 1;

    n = niyah_dalw_search_json(h, "weather", 1, 10u, out, sizeof out, &st);
    CHECK(n > 0 && st == NIYAH_DALW_OK && (size_t)n == strlen(out));
    CHECK(strncmp(out, "{\"query\":\"weather\"", 17) == 0);
    CHECK(strstr(out, "\"count\":1,") != NULL);
    CHECK(strstr(out, "\"document_id\":\"") != NULL);
    CHECK(strstr(out, "\"receipt\":{") != NULL);
    CHECK(n > 0 && out[n - 1] == '}');

    n = niyah_dalw_search_json(h, "weather \xED\xA0\x80", 1, 1u, out, sizeof out, &st);
    CHECK(n > 0 && st == NIYAH_DALW_OK && strstr(out, "weather ???") != NULL);
    CHECK(strstr(out, "\xED") == NULL && strstr(out, "\"count\":1") != NULL);
    n = niyah_dalw_search_json(h, "weather \xEF\xBF\xBD", 1, 1u, out, sizeof out, &st);
    CHECK(n > 0 && strstr(out, "weather \xEF\xBF\xBD") != NULL);

    n = niyah_dalw_search_json(h, "zzzzqqq", 1, 10u, out, sizeof out, &st);
    CHECK(n > 0 && st == NIYAH_DALW_OK && strstr(out, "\"count\":0,\"hits\":[]") != NULL);

    n = niyah_dalw_search_json(h, "", 1, 10u, out, sizeof out, &st);
    CHECK(n == -1 && st == NIYAH_DALW_E_ARG);

    n = niyah_dalw_search_json(h, "weather", 1, 10u, tiny, sizeof tiny, &st);
    CHECK(n == -1 && st == NIYAH_DALW_E_MEM && tiny[0] == '\0');

    n = niyah_dalw_search_json(NULL, "weather", 1, 10u, out, sizeof out, &st);
    CHECK(n == -1 && st == NIYAH_DALW_E_ARG);

    n = niyah_dalw_search_json(h, "weather", 1, 10u, out, sizeof out, NULL);
    CHECK(n > 0);
    if (n > 0 && getenv("NIYAH_JSON_PRINT")) puts(out);

    int length = niyah_dalw_search_json(h, "longtitle", 1, 1, NULL, 0, &st);
    CHECK(length > 20000 && st == NIYAH_DALW_OK);
    if (length > 20000) {
        char *large = malloc((size_t)length + 1);
        CHECK(large != NULL);
        if (large) {
            CHECK(niyah_dalw_search_json(h, "longtitle", 1, 1, large, (size_t)length, &st) == -1);
            CHECK(st == NIYAH_DALW_E_MEM && large[0] == '\0');
            CHECK(niyah_dalw_search_json(h, "longtitle", 1, 1, large, (size_t)length + 1, &st) == length);
            CHECK(st == NIYAH_DALW_OK && strstr(large, title) != NULL && strstr(large, "\"id\":\"") != NULL);
            free(large);
        }
    }
    niyah_dalw_close(h);
    remove(path);
    puts(fails ? "JSON TESTS FAILED" : "JSON TESTS PASSED");
    return fails != 0;
}
