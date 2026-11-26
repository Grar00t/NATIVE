#include "capability.h"
#include "object_id.h"
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(ribat_u64) == 8, "Ribat needs 64-bit counters");
_Static_assert(sizeof(ribat_u32) == 4, "SHA-256 needs 32-bit words");

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); return 1; \
} } while (0)

static int digest_matches(const ribat_u8 bytes[32], const char *expected) {
    static const char hex[] = "0123456789abcdef";
    char text[65];
    for (size_t i = 0; i < 32; ++i) {
        text[2*i] = hex[bytes[i] >> 4];
        text[2*i+1] = hex[bytes[i] & 15];
    }
    text[64] = '\0';
    return strcmp(text, expected) == 0;
}

static int test_sha(void) {
    static const struct { unsigned count; const char *hex; } vectors[] = {
        {0,  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {55, "02779466cdec163811d078815c633f21901413081449002f24aa3e80f0b88ef7"},
        {56, "d4817aa5497628e7c77e6b606107042bbba3130888c5f47a375e6179be789fbb"},
        {63, "c7723fa1e0127975e49e62e753db53924c1bd84b8ac1ac08df78d09270f3d971"},
        {64, "f5a5fd42d16a20302798ef6ed309979b43003d2320d9f0e8ea9831a92759fb4b"},
        {65, "98ce42deef51d40269d542f5314bef2c7468d401ad5d85168bfab4c0108f75f7"}
    };
    /* OpenSSL SHA256_Update on 2^32 - 64 zero bytes, at a block boundary. */
    static const ribat_u32 state[8] = {
        0x8e5d5c75U, 0x160cb1a0U, 0x490dab48U, 0x6b3cb6f4U,
        0x3e5727a3U, 0x5d1b35d5U, 0x1e630964U, 0x239c1d85U
    };
    const ribat_u8 zero[65] = {0};
    ribat_u8 digest[32];
    RibatSha256 ctx;
    for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; ++i) {
        CHECK(ribat_sha256(zero, vectors[i].count, digest) == 0);
        CHECK(digest_matches(digest, vectors[i].hex));
    }
    ribat_sha256_init(&ctx);
    memcpy(ctx.h, state, sizeof state);
    ctx.total = (UINT64_C(1) << 32) - 64;
    CHECK(ribat_sha256_update(&ctx, zero, sizeof zero) == 0);
    CHECK(ctx.total == (UINT64_C(1) << 32) + 1);
    ribat_sha256_final(&ctx, digest);
    CHECK(digest_matches(digest, "fbb82f7b353676bb562eb82157fcf0ea42c36492ca13ee56dbf82c08b6802c5c"));

    ribat_sha256_init(&ctx);
    ctx.total = UINT64_MAX >> 3;
    CHECK(ribat_sha256_update(&ctx, zero, 1) == -1);
    CHECK(ctx.total == (UINT64_MAX >> 3) && ctx.used == 0);
    return 0;
}

static int test_object_capability(void) {
    const ribat_u8 payload[] = {0, 0x80, 0xff};
    const ribat_u8 owner[16] = {1};
    ribat_u8 id[32];
    RibatCapTable table;
    RibatCapability issued, child;
    ribat_u32 guard, next;
    CHECK(ribat_object_id("BIN", 0x1234, payload, sizeof payload, id) == 0);
    CHECK(digest_matches(id, "62889425a806e4f12f3ea27204c0e78ceb2ecbd2f52d5dab0e604ceecd3475b1"));
    CHECK(ribat_cap_table_init(&table, owner, UINT32_MAX) == RIBAT_CAP_OK);
    CHECK(ribat_cap_owner_restart(&table) == RIBAT_CAP_OK);
    CHECK(table.owner_version == (UINT64_C(1) << 32));
    table.guards[0].version = UINT64_MAX - 1;
    CHECK(ribat_cap_guard_create(&table, RIBAT_CAP_NO_PARENT, &guard) == RIBAT_CAP_OK);
    CHECK(guard == 0 && table.guards[guard].version == UINT64_MAX);
    CHECK(ribat_cap_issue(&table, guard, id, RIBAT_CAP_READ | RIBAT_CAP_DELEGATE, &issued) == RIBAT_CAP_OK);
    CHECK(ribat_cap_delegate(&issued, RIBAT_CAP_READ, &child) == RIBAT_CAP_OK);
    CHECK(memcmp(child.resource_id, id, sizeof id) == 0);
    CHECK(ribat_cap_validate(&table, &child, RIBAT_CAP_READ) == RIBAT_CAP_OK);
    CHECK(ribat_cap_validate(&table, &child, RIBAT_CAP_WRITE) == RIBAT_CAP_ERR_RIGHTS);
    CHECK(ribat_cap_guard_revoke(&table, guard) == RIBAT_CAP_OK);
    CHECK(ribat_cap_validate(&table, &child, RIBAT_CAP_READ) == RIBAT_CAP_ERR_REVOKED);
    CHECK(ribat_cap_guard_create(&table, RIBAT_CAP_NO_PARENT, &next) == RIBAT_CAP_OK);
    CHECK(next != guard);
    CHECK(ribat_cap_owner_restart(&table) == RIBAT_CAP_OK);
    CHECK(ribat_cap_validate(&table, &child, RIBAT_CAP_READ) == RIBAT_CAP_ERR_OWNER);
    for (size_t i = 0; i < RIBAT_CAP_GUARDS_MAX; ++i) table.guards[i].version = UINT64_MAX;
    next = RIBAT_CAP_NO_PARENT;
    CHECK(ribat_cap_guard_create(&table, RIBAT_CAP_NO_PARENT, &next) == RIBAT_CAP_ERR_VERSION_EXHAUSTED);
    CHECK(next == RIBAT_CAP_NO_PARENT);
    CHECK(ribat_cap_table_init(&table, owner, UINT64_MAX) == RIBAT_CAP_OK);
    CHECK(ribat_cap_owner_restart(&table) == RIBAT_CAP_ERR_VERSION_EXHAUSTED);
    CHECK(table.owner_version == UINT64_MAX);
    return 0;
}

int main(void) {
    return test_sha() || test_object_capability();
}
