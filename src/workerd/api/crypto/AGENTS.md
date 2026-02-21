# src/workerd/api/crypto/

## OVERVIEW

WebCrypto API + Node.js crypto C++ implementations over BoringSSL. `crypto.h` defines public JSG types (`CryptoKey`, `SubtleCrypto`, `CryptoKeyUsageSet`). `impl.h` defines internal `CryptoKey::Impl` base class with per-algorithm static `ImportFunc`/`GenerateFunc` dispatch. Algorithm files implement `Impl` subclasses. `OSSLCALL()` macro wraps all BoringSSL calls with error translation.

## FILE MAP

| File                                             | Contents                                                                                              |
| ------------------------------------------------ | ----------------------------------------------------------------------------------------------------- |
| `crypto.h/c++`                                   | Public API: `SubtleCrypto`, `CryptoKey`, `CryptoKeyPair`, `DigestStream`, `CryptoKeyUsageSet` bitmask |
| `impl.h/c++`                                     | Internal base: `CryptoKey::Impl`, `interpretAlgorithmParam()`, OpenSSL error helpers, `OSSLCALL`      |
| `keys.h/c++`                                     | `AsymmetricKeyCryptoKeyImpl` base; `KeyEncoding`/`KeyFormat`/`KeyType` enums; generic import/export   |
| `aes.c++`                                        | AES-CBC, AES-CTR, AES-GCM, AES-KW                                                                     |
| `rsa.h/c++`                                      | RSA-OAEP, RSA-PSS, RSASSA-PKCS1-v1_5                                                                  |
| `ec.h/c++`                                       | ECDSA, ECDH, EdDSA (Ed25519/X25519)                                                                   |
| `dh.h/c++`                                       | Diffie-Hellman key exchange (Node.js compat)                                                          |
| `digest.h/c++`                                   | Hash/digest: SHA-\*, MD5, Blake2b512/2s256                                                            |
| `kdf.h` + `hkdf.c++`, `pbkdf2.c++`, `scrypt.c++` | KDF declarations in header; each algo in own .c++                                                     |
| `jwk.h/c++`                                      | JWK import/export helpers                                                                             |
| `x509.h/c++`                                     | X.509 certificate parsing (Node.js compat)                                                            |
| `spkac.h/c++`                                    | SPKAC/Netscape SPKI (Node.js compat)                                                                  |
| `prime.h/c++`                                    | Prime generation/checking (Node.js compat)                                                            |
| `crc-impl.h/c++`                                 | CRC32/CRC32C (non-crypto checksum)                                                                    |
| `endianness.h/c++`                               | Byte-swap utilities                                                                                   |
| `impl-test.c++`, `aes-test.c++`                  | C++ unit tests                                                                                        |
