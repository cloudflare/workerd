// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Helper worker for content-encoding-chain-test. Serves pre-compressed payloads with
// encodeBody: 'manual' so the wire bytes are exactly the embedded payloads. All payloads
// compress DATA below; they were generated with Node's zlib (gzipSync / brotliCompressSync).
//
// The same module runs as two services: "ce-chain-helper" (brotli enabled, has a SELF
// binding to itself) and "ce-chain-nobr" (brotli disabled, has a PEER binding to the
// helper). The /nobr* routes only run in the latter, where fetches through PEER decode
// according to *this* worker's flags.

const DATA = 'The quick brown fox jumps over the lazy dog. '.repeat(32);

// gzip(DATA)
const GZ =
  'H4sIAAAAAAAAEwvJSFUoLM1MzlZIKsovz1NIy69QyCrNLShWyC9LLVIoyUhVyEmsqlRIyU/XUwgZVTyqeFTxqOJRxfRSDABOoPepoAUAAA==';
// gzip(gzip(DATA))
const GZGZ =
  'H4sIAAAAAAAAE5Pv5mAAA2Hukx6hGjpnfc6FeWid0j8f7HF6fcAJrbO6GmEn9L11gzROeoSe8FyzKsTjpP/1YA7JUJtVFSEfVzwKPPoliIfBb8H3lQtYGRgAFoN6Ek8AAAA=';
// brotli(DATA)
const BR =
  'G58FiCwOeNPQlV2XELsXK6nK0JLMjK1BXObyNsgZnp4Ke4MNOHBIIG8kv0GnFc4cHieqKTjCaWmfBgM=';
// brotli(gzip(DATA)), i.e. Content-Encoding: gzip, br
const BRGZ =
  'CyeAH4sIAAAAAAAAEwvJSFUoLM1MzlZIKsovz1NIy69QyCrNLShWyC9LLVIoyUhVyEmsqlRIyU/XUwgZVTyqeFTxqOJRxfRSDABOoPepoAUAAAM=';
// gzip applied five times to DATA, i.e. exactly the decode cap
const GZ5 =
  'H4sIAAAAAAAAEwGKAHX/H4sIAAAAAAAAE5Pv5mAAA+HJ758lMDA/PPl+vqqBgfq+2TrbA05p7f0Wk5167I73t0/5XKuz+AvW3d3jrRX49/S9S2nC+1dNq5RPMsuWrdzz5P91ln2TGTc7u4o+PZy7I6duZdkW+TP2P2q1yluWsDRHFE+dYH8hhcEv+/+UJKBlAGro4pV1AAAATTwn7ooAAAA=';

function decode(b64) {
  return Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
}

const PAYLOADS = {
  'single-gzip': { body: GZ, ce: 'gzip' },
  'single-br': { body: BR, ce: 'br' },
  'double-gzip': { body: GZGZ, ce: 'gzip, gzip' },
  'gzip-br': { body: BRGZ, ce: 'gzip, br' },
  'identity-chain': { body: GZ, ce: 'identity, gzip, identity' },
  'ows-chain': { body: BRGZ, ce: 'gzip\t,  br' },
  'unknown-chain': { body: GZ, ce: 'gzip, deflate' },
  'unknown-single': { body: GZ, ce: 'deflate' },
  'unknown-mid': { body: GZ, ce: 'gzip, deflate, gzip' },
  'empty-segment': { body: BRGZ, ce: 'gzip,,br' },
  'case-single': { body: GZ, ce: 'GZIP' },
  'case-chain': { body: GZGZ, ce: 'gzip, GZIP' },
  'cap-boundary': { body: GZ5, ce: 'gzip, gzip, gzip, gzip, gzip' },
  'too-long': { body: GZGZ, ce: 'gzip, gzip, gzip, gzip, gzip, gzip' },
  'cap-identity': {
    body: GZ,
    ce: 'identity, identity, identity, identity, identity, gzip',
  },
};

async function bodyOf(response) {
  return new Response(await response.arrayBuffer());
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const [, route, name] = url.pathname.split('/');

    if (route === 'raw') {
      const p = PAYLOADS[name];
      if (p === undefined) return new Response('not found', { status: 404 });
      return new Response(decode(p.body), { encodeBody: 'manual' });
    }

    if (route === 'p') {
      if (name === 'dup-headers') {
        // Two separate Content-Encoding headers rather than one comma-separated value.
        const headers = new Headers();
        headers.append('Content-Encoding', 'gzip');
        headers.append('Content-Encoding', 'gzip');
        return new Response(decode(GZGZ), { encodeBody: 'manual', headers });
      }
      const p = PAYLOADS[name];
      if (p === undefined) return new Response('not found', { status: 404 });
      return new Response(decode(p.body), {
        encodeBody: 'manual',
        headers: { 'Content-Encoding': p.ce },
      });
    }

    if (route === 'proxy') {
      // Return the fetched Response as-is; the body must survive unmodified.
      return env.SELF.fetch(`http://helper/p/${name}`);
    }

    if (route === 'out') {
      if (name === 'auto-chain') {
        return new Response(DATA, {
          headers: { 'Content-Encoding': 'gzip, gzip' },
        });
      }
      if (name === 'auto-br-chain') {
        return new Response(DATA, {
          headers: { 'Content-Encoding': 'gzip, br' },
        });
      }
      if (name === 'manual-chain') {
        return new Response(decode(GZGZ), {
          encodeBody: 'manual',
          headers: { 'Content-Encoding': 'gzip, gzip' },
        });
      }
      return new Response('not found', { status: 404 });
    }

    // The /nobr* routes run in the ce-chain-nobr service (no brotli): the fetch through
    // PEER decodes with brotli disabled, and the body is returned as plain bytes with no
    // Content-Encoding, so the test worker sees exactly what this worker's decoder
    // produced.
    if (route === 'nobr') {
      return bodyOf(await env.PEER.fetch(`http://helper/p/${name}`));
    }
    if (route === 'nobr-raw') {
      return bodyOf(await env.PEER.fetch(`http://helper/raw/${name}`));
    }
    if (route === 'nobr-out') {
      return bodyOf(await env.PEER.fetch(`http://helper/out/${name}`));
    }

    return new Response('not found', { status: 404 });
  },
};
