// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>

// Compares the C++ and TypeScript implementations of `node-internal:buffer`,
// selected by the NODEJS_BUFFER_TS autogate, over common node:buffer and
// node:string_decoder operations.

namespace workerd {
namespace {

constexpr kj::StringPtr kScript = R"SCRIPT(
import { Buffer, transcode, isUtf8, isAscii } from 'node:buffer';
import { StringDecoder } from 'node:string_decoder';

const ascii = (n) => 'abcdefghijklmnopqrstuvwxyz0123456789'.repeat(Math.ceil(n / 36)).slice(0, n);
const mixed = (n) => 'aé€😀'.repeat(Math.ceil(n / 4)).slice(0, n);
const s16 = ascii(16), s1k = ascii(1024), m1k = mixed(1024), s64k = ascii(65536);
const b16 = Buffer.from(s16), b1k = Buffer.from(s1k), b64k = Buffer.from(s64k);
const b1kCopy = Buffer.from(b1k), b64kCopy = Buffer.from(b64k);
const u1k = Buffer.from(m1k);
const b64str1k = b1k.toString('base64'), hexstr1k = b1k.toString('hex');
const http = Buffer.from(('GET / HTTP/1.1\r\nHost: example.com\r\nX-Header: ' + ascii(40)).repeat(20) + '\r\n\r\n');
const needleLong = s64k.slice(40000, 40040);
const parts = Array.from({length: 16}, (_, i) => Buffer.from(ascii(64 + i)));
const dest = Buffer.alloc(4096);
const u16buf = Buffer.from(s1k, 'utf16le');
let sink;

export const ops = {
  'from-utf8-16': () => Buffer.from(s16),
  'from-utf8-1k': () => Buffer.from(s1k),
  'from-utf8-mixed-1k': () => Buffer.from(m1k),
  'from-latin1-1k': () => Buffer.from(s1k, 'latin1'),
  'from-base64-1k': () => Buffer.from(b64str1k, 'base64'),
  'from-hex-1k': () => Buffer.from(hexstr1k, 'hex'),
  'from-utf16-1k': () => Buffer.from(s1k, 'utf16le'),
  'byteLength-mixed-1k': () => Buffer.byteLength(m1k),
  'write-utf8-1k': () => dest.write(s1k),
  'toString-utf8-16': () => b16.toString(),
  'toString-utf8-1k': () => b1k.toString(),
  'toString-utf8-mixed-1k': () => u1k.toString(),
  'toString-latin1-1k': () => b1k.toString('latin1'),
  'toString-base64-1k': () => b1k.toString('base64'),
  'toString-hex-1k': () => b1k.toString('hex'),
  'toString-utf16-1k': () => u16buf.toString('utf16le'),
  'equals-16': () => b16.equals(Buffer.from(b16)),
  'equals-1k': () => b1k.equals(b1kCopy),
  'equals-64k': () => b64k.equals(b64kCopy),
  'compare-1k': () => Buffer.compare(b1k, b1kCopy),
  'concat-16x64': () => Buffer.concat(parts),
  'indexOf-crlf-http': () => http.indexOf('\r\n\r\n'),
  'indexOf-byte-64k': () => b64k.indexOf(0x21),
  'indexOf-short-64k': () => b64k.indexOf('xyz!'),
  'indexOf-long-64k': () => b64k.indexOf(needleLong),
  'indexOf-buf-long-64k': () => b64k.indexOf(Buffer.from(needleLong)),
  'lastIndexOf-short-64k': () => b64k.lastIndexOf('abc', 10),
  'includes-utf16-1k': () => u16buf.includes('zzz', 0, 'utf16le'),
  'fill-str-1k': () => dest.fill('abc', 0, 1024),
  'fill-hex-1k': () => dest.fill('abcd', 0, 1024, 'hex'),
  'swap16-1k': () => b1k.swap16(),
  'swap64-1k': () => b1k.swap64(),
  'isUtf8-1k': () => isUtf8(u1k),
  'isAscii-1k': () => isAscii(b1k),
  'transcode-1k': () => transcode(b1k, 'utf8', 'utf16le'),
  'decoder-utf8-chunks': () => {
    const d = new StringDecoder('utf8');
    let r = '';
    for (let i = 0; i < u1k.length; i += 7) r += d.write(u1k.subarray(i, i + 7));
    return r + d.end();
  },
  'decoder-utf8-whole': () => new StringDecoder('utf8').write(b1k),
};

export default {
  async fetch(request) {
    const url = new URL(request.url);
    const fn = ops[url.searchParams.get('op')];
    const n = parseInt(url.searchParams.get('n'));
    for (let i = 0; i < n; i++) sink = fn();
    return new Response(String(sink?.length ?? sink));
  },
};
)SCRIPT"_kj;

struct Case {
  kj::StringPtr name;
  int iterations;
};

constexpr Case kCases[] = {
  {"from-utf8-16"_kj, 1000},
  {"from-utf8-1k"_kj, 1000},
  {"from-utf8-mixed-1k"_kj, 1000},
  {"from-latin1-1k"_kj, 1000},
  {"from-base64-1k"_kj, 1000},
  {"from-hex-1k"_kj, 1000},
  {"from-utf16-1k"_kj, 1000},
  {"byteLength-mixed-1k"_kj, 1000},
  {"write-utf8-1k"_kj, 1000},
  {"toString-utf8-16"_kj, 1000},
  {"toString-utf8-1k"_kj, 1000},
  {"toString-utf8-mixed-1k"_kj, 1000},
  {"toString-latin1-1k"_kj, 1000},
  {"toString-base64-1k"_kj, 1000},
  {"toString-hex-1k"_kj, 1000},
  {"toString-utf16-1k"_kj, 1000},
  {"equals-16"_kj, 1000},
  {"equals-1k"_kj, 1000},
  {"equals-64k"_kj, 100},
  {"compare-1k"_kj, 1000},
  {"concat-16x64"_kj, 1000},
  {"indexOf-crlf-http"_kj, 1000},
  {"indexOf-byte-64k"_kj, 100},
  {"indexOf-short-64k"_kj, 100},
  {"indexOf-long-64k"_kj, 100},
  {"indexOf-buf-long-64k"_kj, 100},
  {"lastIndexOf-short-64k"_kj, 1000},
  {"includes-utf16-1k"_kj, 1000},
  {"fill-str-1k"_kj, 1000},
  {"fill-hex-1k"_kj, 1000},
  {"swap16-1k"_kj, 1000},
  {"swap64-1k"_kj, 1000},
  {"isUtf8-1k"_kj, 1000},
  {"isAscii-1k"_kj, 1000},
  {"transcode-1k"_kj, 1000},
  {"decoder-utf8-chunks"_kj, 1000},
  {"decoder-utf8-whole"_kj, 1000},
};

void benchBuffer(benchmark::State& state, bool ts, const Case& c) {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setNodeJsCompat(true);
  flags.setNodeJsCompatV2(true);
  kj::StringPtr gates[] = {"nodejs-buffer-ts"_kj};
  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .autogates = ts ? kj::heapArray<kj::StringPtr>(gates) : kj::heapArray<kj::StringPtr>(0),
    .mainModuleSource = kScript,
  });
  auto url = kj::str("http://example.com?op=", c.name, "&n=", c.iterations);
  // Warm up so both implementations are measured after JIT tier-up.
  for (int i = 0; i < 20; i++) fixture.runRequest(kj::HttpMethod::GET, url, ""_kj);
  for (auto _: state) {
    benchmark::DoNotOptimize(fixture.runRequest(kj::HttpMethod::GET, url, ""_kj));
  }
  state.SetItemsProcessed(state.iterations() * c.iterations);
}

[[maybe_unused]] int registered = [] {
  for (auto& c: kCases) {
    for (bool ts: {false, true}) {
      benchmark::RegisterBenchmark(kj::str(c.name, ts ? "/ts" : "/cpp").cStr(),
          [ts, &c](benchmark::State& state) { benchBuffer(state, ts, c); })
          ->Unit(benchmark::kMicrosecond);
    }
  }
  return 0;
}();

}  // namespace
}  // namespace workerd
