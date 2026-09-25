// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual, deepStrictEqual } from 'node:assert';
import { gunzipSync, brotliDecompressSync } from 'node:zlib';

// Tests for stacked Content-Encoding values (https://github.com/cloudflare/workerd/issues/7051).
// The helper service serves pre-compressed payloads with encodeBody: 'manual', so the bytes on
// the wire are exactly the embedded payloads. Decoding happens on this side of the fetch.

const DATA = 'The quick brown fox jumps over the lazy dog. '.repeat(32);

async function text(env, path) {
  const resp = await env.HELPER.fetch(`http://helper${path}`);
  return await resp.text();
}

async function bytes(fetcher, path) {
  const resp = await fetcher.fetch(`http://helper${path}`);
  return new Uint8Array(await resp.arrayBuffer());
}

// Expects the body to arrive decoded all the way down to the original text.
async function expectDecoded(env, name) {
  strictEqual(await text(env, `/p/${name}`), DATA);
}

// Expects the two paths to produce byte-identical bodies through the given fetcher.
async function expectSameBytes(fetcher, pathA, pathB) {
  deepStrictEqual(await bytes(fetcher, pathA), await bytes(fetcher, pathB));
}

// Expects the body to arrive byte-identical to the stored payload (no decoding at all).
async function expectPassthrough(env, name) {
  await expectSameBytes(env.HELPER, `/p/${name}`, `/raw/${name}`);
}

export const singleTokenBehaviorUnchanged = {
  async test(ctrl, env) {
    // Pre-existing single-token behavior must not change.
    await expectDecoded(env, 'single-gzip');
    await expectDecoded(env, 'single-br');
    await expectPassthrough(env, 'unknown-single');
    // Token matching is case-sensitive, as it always has been for single values.
    await expectPassthrough(env, 'case-single');
  },
};

export const doubleGzipChain = {
  async test(ctrl, env) {
    await expectDecoded(env, 'double-gzip');
  },
};

export const gzipBrotliChain = {
  async test(ctrl, env) {
    await expectDecoded(env, 'gzip-br');
  },
};

export const identityTokensSkipped = {
  async test(ctrl, env) {
    // "identity, gzip, identity" only requires undoing the gzip layer.
    await expectDecoded(env, 'identity-chain');
  },
};

export const emptySegmentsIgnored = {
  async test(ctrl, env) {
    // Empty list elements are ignored per RFC 9110 5.6.1.2: "gzip,,br" decodes the same as
    // "gzip, br".
    await expectDecoded(env, 'empty-segment');
  },
};

export const optionalWhitespaceTrimmed = {
  async test(ctrl, env) {
    // OWS around the commas must be ignored per RFC 9110.
    await expectDecoded(env, 'ows-chain');
  },
};

export const unknownTokenDisablesDecoding = {
  async test(ctrl, env) {
    // An unsupported coding anywhere in the list means the body is passed through untouched,
    // consistent with the single unknown-value behavior.
    await expectPassthrough(env, 'unknown-chain');
    await expectPassthrough(env, 'case-chain');
  },
};

export const probeDeflateMid = {
  async test(ctrl, env) {
    // An unsupported coding in the *middle* of the list (not just the last position) also
    // disables decoding for the whole body.
    await expectPassthrough(env, 'unknown-mid');
  },
};

export const probeCapBoundary = {
  async test(ctrl, env) {
    // Exactly five codings sit on the cap and decode; a sixth pushes the list over and the
    // body passes through.
    await expectDecoded(env, 'cap-boundary');
    await expectPassthrough(env, 'too-long');
  },
};

export const capCountsEffectiveCodings = {
  async test(ctrl, env) {
    // The cap counts codings that actually transform the body: five "identity" tokens plus a
    // gzip is one effective coding, well under the cap, so it decodes.
    await expectDecoded(env, 'cap-identity');
  },
};

export const duplicateHeadersJoined = {
  async test(ctrl, env) {
    // Two separate Content-Encoding headers are equivalent to one comma-separated list.
    await expectDecoded(env, 'dup-headers');
  },
};

export const proxiedChainStillDecodes = {
  async test(ctrl, env) {
    // A worker that returns a fetched Response unmodified must not corrupt the body.
    strictEqual(await text(env, '/proxy/gzip-br'), DATA);
    await expectSameBytes(env.HELPER, '/proxy/unknown-chain', '/raw/unknown-chain');
  },
};

export const proxiedChainKeepsContentLength = {
  async test(ctrl, env) {
    // HELPER_WIRE reaches the helper over a real (loopback) HTTP socket, so Content-Length
    // vs. chunked is actual wire behavior rather than an in-process shortcut. A proxied
    // multi-coding body passes through with its chain intact, so its encoded length is known
    // and must survive as Content-Length.
    const raw = await bytes(env.HELPER, '/raw/gzip-br');
    const resp = await env.HELPER_WIRE.fetch('http://helper/proxy/gzip-br');
    strictEqual(resp.headers.get('content-length'), String(raw.byteLength));
    strictEqual(resp.headers.get('transfer-encoding'), null);
    strictEqual(await resp.text(), DATA);
  },
};

export const constructedChainHasNoContentLength = {
  async test(ctrl, env) {
    // A worker-constructed Response with a stacked Content-Encoding header is genuinely
    // re-encoded on send: no length can be advertised, so the body goes out chunked.
    const resp = await env.HELPER_WIRE.fetch('http://helper/out/auto-chain');
    strictEqual(resp.headers.get('content-length'), null);
    strictEqual(await resp.text(), DATA);
  },
};

export const outputChainEncoded = {
  async test(ctrl, env) {
    // A Response constructed with a stacked Content-Encoding header and an identity body gets
    // encoded on send, mirroring the existing single-value behavior.
    strictEqual(await text(env, '/out/auto-chain'), DATA);
    strictEqual(await text(env, '/out/auto-br-chain'), DATA);
  },
};

export const probeEncoderByteLayout = {
  async test(ctrl, env) {
    // Byte-level check of the *encode* path, independent of workerd's decoder (a symmetric
    // layering bug would round-trip cleanly through /out tests alone). The brotli-disabled
    // service fetches /out/auto-br-chain from the helper and passes the body through raw
    // (its "gzip, br" is an unknown chain there), and this side decodes with node:zlib:
    // the bytes must be br(gzip(DATA)), i.e. the codings applied in list order.
    const encoded = await bytes(env.NOBR, '/nobr-out/auto-br-chain');
    const decoded = gunzipSync(brotliDecompressSync(encoded));
    strictEqual(decoded.toString('utf8'), DATA);
  },
};

export const manualBodyNotReencoded = {
  async test(ctrl, env) {
    // encodeBody: 'manual' keeps meaning "the app already encoded the body": the pre-compressed
    // payload goes out untouched and decodes to the original text exactly once on this side.
    strictEqual(await text(env, '/out/manual-chain'), DATA);
  },
};

export const brotliDisabledMakesChainUnknown = {
  async test(ctrl, env) {
    // In a worker without brotli_content_encoding, "br" in a chain is an unknown token, so the
    // whole body passes through; a single "br" behaves the same as before.
    await expectSameBytes(env.NOBR, '/nobr/gzip-br', '/nobr-raw/gzip-br');
    await expectSameBytes(env.NOBR, '/nobr/single-br', '/nobr-raw/single-br');
  },
};
