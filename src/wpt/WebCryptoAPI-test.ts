// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { type TestRunnerConfig } from 'harness/harness';

export default {
  'algorithm-discards-context.https.window.js': {
    comment: 'Secure context is only relevant in browsers',
    omittedTests: true,
  },
  'crypto_key_cached_slots.https.any.js': {
    comment: 'Investigate this',
    expectedFailures: [
      'CryptoKey.algorithm getter returns cached object',
      'CryptoKey.usages getter returns cached object',
    ],
  },

  'derive_bits_keys/cfrg_curves_bits.js': {},
  'derive_bits_keys/cfrg_curves_bits_curve25519.https.any.js': {},
  'derive_bits_keys/cfrg_curves_bits_curve448.https.any.js': {
    comment:
      'X448 is not supported by BoringSSL and is intentionally disabled.',
    omittedTests: true,
  },
  'derive_bits_keys/cfrg_curves_bits_fixtures.js': {},
  'derive_bits_keys/cfrg_curves_keys.js': {},
  'derive_bits_keys/cfrg_curves_keys_curve25519.https.any.js': {},
  'derive_bits_keys/cfrg_curves_keys_curve448.https.any.js': {
    comment:
      'X448 is not supported by BoringSSL and is intentionally disabled.',
    omittedTests: true,
  },
  'derive_bits_keys/derive_key_and_encrypt.https.any.js': {},
  'derive_bits_keys/derive_key_and_encrypt.js': {},
  'derive_bits_keys/derived_bits_length.https.any.js': {},
  'derive_bits_keys/derived_bits_length.js': {},
  'derive_bits_keys/derived_bits_length_testcases.js': {
    comment:
      "This is a resource file but it's not in the resources/ directory; no tests in here",
    omittedTests: true,
  },
  'derive_bits_keys/derived_bits_length_vectors.js': {},
  'derive_bits_keys/ecdh_bits.https.any.js': {},
  'derive_bits_keys/ecdh_bits.js': {},
  'derive_bits_keys/ecdh_keys.https.any.js': {},
  'derive_bits_keys/ecdh_keys.js': {},
  'derive_bits_keys/hkdf.https.any.js': {
    comment: 'Cannot cope with this many iterations, keeps timing out',
    omittedTests: [/with 100000 iterations/],
  },
  'derive_bits_keys/hkdf.js': {},
  'derive_bits_keys/hkdf_vectors.js': {},
  'derive_bits_keys/pbkdf2.https.any.js': {
    comment: 'Cannot cope with this many iterations, keeps timing out',
    omittedTests: [/with 100000 iterations/],
  },
  'derive_bits_keys/pbkdf2.js': {},
  'derive_bits_keys/pbkdf2_vectors.js': {},

  'digest/digest.https.any.js': {
    comment: 'They expect TypeError, we have NotSupportedError',
    expectedFailures: [
      'empty algorithm object with empty',
      'empty algorithm object with short',
      'empty algorithm object with medium',
      'empty algorithm object with long',
    ],
  },

  'encrypt_decrypt/aes.js': {},
  'encrypt_decrypt/aes_cbc.https.any.js': {},
  'encrypt_decrypt/aes_cbc_vectors.js': {},
  'encrypt_decrypt/aes_ctr.https.any.js': {},
  'encrypt_decrypt/aes_ctr_vectors.js': {},
  'encrypt_decrypt/aes_gcm.https.any.js': {},
  'encrypt_decrypt/aes_gcm_256_iv.https.any.js': {},
  'encrypt_decrypt/aes_gcm_256_iv_fixtures.js': {},
  'encrypt_decrypt/aes_gcm_96_iv_fixtures.js': {},
  'encrypt_decrypt/aes_gcm_vectors.js': {},
  'encrypt_decrypt/rsa.js': {},
  'encrypt_decrypt/rsa_oaep.https.any.js': {},
  'encrypt_decrypt/rsa_vectors.js': {},

  'generateKey/failures.js': {},
  'generateKey/failures_AES-CBC.https.any.js': {
    comment: 'Wrong type of error returned',
    expectedFailures: [/^(Empty|Bad) algorithm:/],
  },
  'generateKey/failures_AES-CTR.https.any.js': {
    comment: 'Wrong type of error returned',
    expectedFailures: [/^(Empty|Bad) algorithm:/],
  },
  'generateKey/failures_AES-GCM.https.any.js': {
    comment: 'Wrong type of error returned',
    expectedFailures: [/^(Empty|Bad) algorithm:/],
  },
  'generateKey/failures_AES-KW.https.any.js': {
    comment: 'Wrong type of error returned',
    expectedFailures: [/^(Empty|Bad) algorithm:/],
  },
  'generateKey/failures_ECDH.https.any.js': {
    comment: 'Wrong type of error returned',
    expectedFailures: [/^(Empty|Bad) algorithm:/],
  },
  'generateKey/failures_ECDSA.https.any.js': {
    comment: 'Wrong type of error returned',
    expectedFailures: [/^(Empty|Bad) algorithm:/],
  },
  'generateKey/failures_Ed25519.https.any.js': {
    comment: 'Wrong type of error returned',
    expectedFailures: [/^(Empty|Bad) algorithm:/],
  },
  'generateKey/failures_Ed448.https.any.js': {
    comment:
      'Ed448 is not supported by BoringSSL and is intentionally disabled.',
    omittedTests: true,
  },
  'generateKey/failures_HMAC.https.any.js': {
    comment: 'Wrong type of error returned',
    expectedFailures: [/^(Empty|Bad) algorithm:/],
  },
  'generateKey/failures_RSA-OAEP.https.any.js': {
    comment: 'Wrong type of error returned',
    expectedFailures: [/^(Empty|Bad) algorithm:/],
  },
  'generateKey/failures_RSA-PSS.https.any.js': {
    comment: 'Wrong type of error returned',
    expectedFailures: [/^(Empty|Bad) algorithm:/],
  },
  'generateKey/failures_RSASSA-PKCS1-v1_5.https.any.js': {
    comment: 'Wrong type of error returned',
    expectedFailures: [/^(Empty|Bad) algorithm:/],
  },
  'generateKey/failures_X25519.https.any.js': {
    comment: 'Wrong type of error returned',
    expectedFailures: [/^(Empty|Bad) algorithm:/],
  },
  'generateKey/failures_X448.https.any.js': {
    comment:
      'X448 is not supported by BoringSSL and is intentionally disabled.',
    omittedTests: true,
  },
  'generateKey/successes.js': {},
  'generateKey/successes_AES-CBC.https.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [/^undefined: /],
  },
  'generateKey/successes_AES-CTR.https.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [/^undefined: /],
  },
  'generateKey/successes_AES-GCM.https.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [/^undefined: /],
  },
  'generateKey/successes_AES-KW.https.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [/^undefined: /],
  },
  'generateKey/successes_ECDH.https.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [/^undefined: /],
  },
  'generateKey/successes_ECDSA.https.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [/^undefined: /],
  },
  'generateKey/successes_Ed25519.https.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [/^undefined: /],
  },
  'generateKey/successes_Ed448.https.any.js': {
    comment:
      'Ed448 is not supported by BoringSSL and is intentionally disabled.',
    omittedTests: true,
  },
  'generateKey/successes_HMAC.https.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [/^undefined: /],
  },
  'generateKey/successes_RSA-OAEP.https.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [/^undefined: /],
  },
  'generateKey/successes_RSA-PSS.https.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [/^undefined: /],
  },
  'generateKey/successes_RSASSA-PKCS1-v1_5.https.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [/^undefined: /],
  },
  'generateKey/successes_X25519.https.any.js': {
    comment: 'TODO investigate this',
    expectedFailures: [/^undefined: /],
  },
  'generateKey/successes_X448.https.any.js': {
    comment:
      'X448 is not supported by BoringSSL and is intentionally disabled.',
    omittedTests: true,
  },

  'getRandomValues.any.js': {},
  'historical.any.js': {
    comment: 'Secure context is only relevant to browsers',
    omittedTests: [
      'Non-secure context window does not have access to crypto.subtle',
      'Non-secure context window does not have access to SubtleCrypto',
      'Non-secure context window does not have access to CryptoKey',
    ],
  },
  'idlharness.https.any.js': {
    comment: 'Test file /resources/WebIDLParser.js not found.',
    disabledTests: true,
  },

  'import_export/crashtests/importKey-unsettled-promise.https.any.js': {},
  'import_export/ec_importKey.https.any.js': {},
  'import_export/ec_importKey_failures_ECDH.https.any.js': {
    comment:
      'OpenSSL call failed: EC_POINT_set_affine_coordinates_GFp(group, point, bigX, bigY, nullptr);',
    expectedFailures: [
      /^Bad key length:/,
      /^Missing JWK 'crv' parameter:/,
      "Invalid 'crv' field: importKey(jwk(private), {name: ECDH, namedCurve: P-256}, true, [deriveKey, deriveBits])",
      "Invalid 'crv' field: importKey(jwk(private), {name: ECDH, namedCurve: P-384}, true, [deriveKey, deriveBits])",
      "Invalid 'crv' field: importKey(jwk(private), {name: ECDH, namedCurve: P-521}, true, [deriveKey, deriveBits])",
    ],
  },
  'import_export/ec_importKey_failures_ECDSA.https.any.js': {
    comment:
      'OpenSSL call failed: EC_POINT_set_affine_coordinates_GFp(group, point, bigX, bigY, nullptr);',
    expectedFailures: [
      /^Bad key length:/,
      /^Missing JWK 'crv' parameter:/,
      "Invalid 'crv' field: importKey(jwk(private), {name: ECDSA, namedCurve: P-256}, true, [sign])",
      "Invalid 'crv' field: importKey(jwk(private), {name: ECDSA, namedCurve: P-384}, true, [sign])",
      "Invalid 'crv' field: importKey(jwk(private), {name: ECDSA, namedCurve: P-521}, true, [sign])",
    ],
  },
  'import_export/ec_importKey_failures_fixtures.js': {},
  'import_export/importKey_failures.js': {},
  'import_export/okp_importKey.js': {},
  'import_export/okp_importKey_Ed25519.https.any.js': {
    comment: 'Investigate this',
    expectedFailures: [
      'Good parameters with JWK alg Ed25519: Ed25519 (jwk, object(kty, crv, x), {name: Ed25519}, true, [verify])',
      'Good parameters with JWK alg Ed25519: Ed25519 (jwk, object(kty, crv, x), Ed25519, true, [verify])',
      'Good parameters with JWK alg Ed25519: Ed25519 (jwk, object(kty, crv, x), {name: Ed25519}, true, [])',
      'Good parameters with JWK alg Ed25519: Ed25519 (jwk, object(kty, crv, x), Ed25519, true, [])',
      'Good parameters with JWK alg Ed25519: Ed25519 (jwk, object(kty, crv, x), {name: Ed25519}, true, [verify, verify])',
      'Good parameters with JWK alg Ed25519: Ed25519 (jwk, object(kty, crv, x), Ed25519, true, [verify, verify])',
      'Good parameters with JWK alg Ed25519: Ed25519 (jwk, object(crv, d, x, kty), {name: Ed25519}, true, [sign])',
      'Good parameters with JWK alg Ed25519: Ed25519 (jwk, object(crv, d, x, kty), Ed25519, true, [sign])',
      'Good parameters with JWK alg Ed25519: Ed25519 (jwk, object(crv, d, x, kty), {name: Ed25519}, true, [sign, sign])',
      'Good parameters with JWK alg Ed25519: Ed25519 (jwk, object(crv, d, x, kty), Ed25519, true, [sign, sign])',
      'Good parameters with JWK alg EdDSA: Ed25519 (jwk, object(kty, crv, x), {name: Ed25519}, true, [verify])',
      'Good parameters with JWK alg EdDSA: Ed25519 (jwk, object(kty, crv, x), Ed25519, true, [verify])',
      'Good parameters with JWK alg EdDSA: Ed25519 (jwk, object(kty, crv, x), {name: Ed25519}, true, [])',
      'Good parameters with JWK alg EdDSA: Ed25519 (jwk, object(kty, crv, x), Ed25519, true, [])',
      'Good parameters with JWK alg EdDSA: Ed25519 (jwk, object(kty, crv, x), {name: Ed25519}, true, [verify, verify])',
      'Good parameters with JWK alg EdDSA: Ed25519 (jwk, object(kty, crv, x), Ed25519, true, [verify, verify])',
      'Good parameters with JWK alg EdDSA: Ed25519 (jwk, object(crv, d, x, kty), {name: Ed25519}, true, [sign])',
      'Good parameters with JWK alg EdDSA: Ed25519 (jwk, object(crv, d, x, kty), Ed25519, true, [sign])',
      'Good parameters with JWK alg EdDSA: Ed25519 (jwk, object(crv, d, x, kty), {name: Ed25519}, true, [sign, sign])',
      'Good parameters with JWK alg EdDSA: Ed25519 (jwk, object(crv, d, x, kty), Ed25519, true, [sign, sign])',
    ],
  },
  'import_export/okp_importKey_Ed448.https.any.js': {
    comment:
      'Ed448 is not supported by BoringSSL and is intentionally disabled.',
    omittedTests: true,
  },
  'import_export/okp_importKey_X25519.https.any.js': {},
  'import_export/okp_importKey_X448.https.any.js': {
    comment:
      'X448 is not supported by BoringSSL and is intentionally disabled.',
    omittedTests: true,
  },
  'import_export/okp_importKey_failures_Ed25519.https.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'Invalid key pair: importKey(jwk(private), {name: Ed25519}, true, [sign])',
      'Invalid key pair: importKey(jwk(private), {name: Ed25519}, true, [sign, sign])',
      "Invalid 'crv' field: importKey(jwk(private), {name: Ed25519}, true, [sign])",
      "Invalid 'crv' field: importKey(jwk (public) , {name: Ed25519}, true, [verify])",
    ],
  },
  'import_export/okp_importKey_failures_Ed448.https.any.js': {
    comment:
      'Ed448 is not supported by BoringSSL and is intentionally disabled.',
    omittedTests: true,
  },
  'import_export/okp_importKey_failures_X25519.https.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'Invalid key pair: importKey(jwk(private), {name: X25519}, true, [deriveKey])',
      'Invalid key pair: importKey(jwk(private), {name: X25519}, true, [deriveBits, deriveKey])',
      'Invalid key pair: importKey(jwk(private), {name: X25519}, true, [deriveBits])',
      'Invalid key pair: importKey(jwk(private), {name: X25519}, true, [deriveKey, deriveBits, deriveKey, deriveBits])',
      "Invalid 'crv' field: importKey(jwk(private), {name: X25519}, true, [deriveKey, deriveBits])",
      "Invalid 'crv' field: importKey(jwk (public) , {name: X25519}, true, [])",
    ],
  },
  'import_export/okp_importKey_failures_X448.https.any.js': {
    comment:
      'X448 is not supported by BoringSSL and is intentionally disabled.',
    omittedTests: true,
  },
  'import_export/okp_importKey_failures_fixtures.js': {},
  'import_export/okp_importKey_fixtures.js': {},
  'import_export/rsa_importKey.https.any.js': {},
  'import_export/symmetric_importKey.https.any.js': {},

  'randomUUID.https.any.js': {},

  'sign_verify/ecdsa.https.any.js': {},
  'sign_verify/ecdsa.js': {},
  'sign_verify/ecdsa_vectors.js': {},
  'sign_verify/eddsa.js': {},
  'sign_verify/eddsa_curve25519.https.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'EdDSA Ed25519 verification failure due to shortened signature',
    ],
  },
  'sign_verify/eddsa_curve448.https.any.js': {
    comment:
      'Ed448 is not supported by BoringSSL and is intentionally disabled.',
    omittedTests: true,
  },
  'sign_verify/eddsa_small_order_points.https.any.js': {
    comment: 'To be investigated',
    expectedFailures: [
      'Ed25519 Verification checks with small-order key of order - Test 0',
      'Ed25519 Verification checks with small-order key of order - Test 1',
      'Ed25519 Verification checks with small-order key of order - Test 2',
      'Ed25519 Verification checks with small-order key of order - Test 7',
      'Ed25519 Verification checks with small-order key of order - Test 11',
      'Ed25519 Verification checks with small-order key of order - Test 12',
      'Ed25519 Verification checks with small-order key of order - Test 13',
    ],
  },
  'sign_verify/eddsa_small_order_points.js': {},
  'sign_verify/eddsa_vectors.js': {},
  'sign_verify/hmac.https.any.js': {},
  'sign_verify/hmac.js': {},
  'sign_verify/hmac_vectors.js': {},
  'sign_verify/rsa.js': {},
  'sign_verify/rsa_pkcs.https.any.js': {},
  'sign_verify/rsa_pkcs_vectors.js': {},
  'sign_verify/rsa_pss.https.any.js': {},
  'sign_verify/rsa_pss_vectors.js': {},

  'util/helpers.js': {},
  'util/worker-report-crypto-subtle-presence.js': {
    comment: 'ReferenceError: postMessage is not defined',
    disabledTests: true,
  },

  'wrapKey_unwrapKey/wrapKey_unwrapKey.https.any.js': {},
  'wrapKey_unwrapKey/wrapKey_unwrapKey_vectors.js': {},
} satisfies TestRunnerConfig;
