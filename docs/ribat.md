# Ribat in NATIVE

The import commit has two parents. Its second parent is the original Ribat commit recorded in components.lock.json. Its ribat/ tree matches the source tree at that commit, including the MIT license and design documents.

## Object identity and capabilities

Call ribat_object_id with a three-byte type, a version from 0 to 65535 and a payload. The SHA-256 input is the 11 ASCII bytes RIBAT/OBJ/1, a zero byte, the three type bytes, the version in two big-endian bytes, the payload length in eight big-endian bytes, then the payload. For type BIN, version 0x1234 and payload 00 80 FF, the digest is 62889425a806e4f12f3ea27204c0e78ceb2ecbd2f52d5dab0e604ceecd3475b1.

Pass the resulting 32 bytes to ribat_cap_issue as resource_id. The capability tests issue a right for this object, delegate read access, revoke the guard and invalidate the old owner generation on restart. The capability table operates in one trusted address space. The network transport, kernel IPC calls and ring-3 servers in the imported documents describe proposed interfaces. The executable Ribat sources implement SHA-256, object identification and the in-memory capability table.

Dalw hashes shard bodies, document spans and receipt bytes with dalw_sha256_update. Its count argument is size_t and it returns void. Ribat accepts a uint64_t count and reports an error if a call would exceed the SHA-256 message length limit. Each component retains its streaming interface and byte format.

## Windows ABI

Rebuild Windows x64 callers against the updated headers. The SHA byte counter, owner version and guard versions use uint64_t. Stored indices and rights use uint32_t. Native structure layouts are unsuitable for file and network serialization.

| Type | Previous MSVC x64 bytes | Current MSVC x64 bytes | Current WSL x86-64 bytes |
| --- | ---: | ---: | ---: |
| ribat_u64 | 4 | 8 | 8 |
| RibatSha256 | 104 | 112 | 112 |
| RibatCapGuard | 12 | 16 | 16 |
| RibatCapTable | 788 | 1048 | 1048 |
| RibatCapability | 64 | 80 | 80 |

Run make -C ribat test for the native checks. The padding fixtures cover 0, 55, 56, 63, 64 and 65 zero bytes. A SHA-256 state from OpenSSL after hashing 2^32 − 64 zero bytes permits an additional 65 bytes to cross the 32-bit byte counter in one test. The expected digest covers the entire 2^32 + 1 byte stream. OpenSSL generated the oracle values independently and is unnecessary for building or running the tests.

## الكمية

The optional Vigenère exercise accepts uppercase ASCII A through Z. The command takes a nonempty repeating key and a text argument. Empty text is valid. It performs no normalization and rejects other bytes, including spaces, before writing any output. Number the letters from 0 to 25. Add the key value modulo 26 to encrypt and subtract it modulo 26 to decrypt.

| Plaintext | Repeated key | Ciphertext |
| --- | --- | --- |
| ATTACKATDAWN | LEMONLEMONLE | LXFOPVEFRNHR |

~~~sh
make build/al-kimiyya
build/al-kimiyya encrypt LEMON ATTACKATDAWN
build/al-kimiyya decrypt LEMON LXFOPVEFRNHR
~~~

This historical cipher has no confidentiality role. Its executable is separate from the object and receipt code.

## Confidential object prerequisites

Confidential storage has no implemented backend in this import. A future writer would encrypt the canonical payload, authenticate the envelope format, type, version and length as associated data, and keep the key under the control of the storage owner. A reader must authenticate the whole envelope before exposing plaintext or creating an object. Authentication failure returns an error and discards pending output.

For a provider implementing RFC 8439, the format needs a unique 96-bit nonce per key. The writer must reserve nonce values durably across crashes and concurrent writers. Recovering an old allocator state requires a new key before writing. Remaining prerequisites are a reviewed cross-platform AEAD provider compatible with the build, an owner key store and a tested nonce allocator. There is no peer key-establishment entry point in this version. [RFC 8439, sections 2.8 and 4](https://www.rfc-editor.org/rfc/rfc8439).
