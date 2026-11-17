// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// URL and URLSearchParams on the standard (ada) path: construction,
// attribute reads and writes, and the search parameter list. Each call does
// COUNT operations, since one costs about as much as the runner's overhead
// per call.

const COUNT = 16;

const kAbsolute = 'https://user:pass@sub.example.com:8080/a/b/c?x=1&y=2#frag';
const kRelative = '../d/e?z=3';
const kQuery = Array.from({ length: 10 }, (_, i) => `k${i}=v${i}`).join('&');

export const construction = {
  bench(b) {
    b.run(`new URL(absolute) x${COUNT}`, () => {
      let url;
      for (let i = 0; i < COUNT; i++) url = new URL(kAbsolute);
      b.blackBox(url);
    });
    b.run(`new URL(relative, base) x${COUNT}`, () => {
      let url;
      for (let i = 0; i < COUNT; i++) url = new URL(kRelative, kAbsolute);
      b.blackBox(url);
    });
    b.run(`new URL(absolute).href x${COUNT}`, () => {
      let href;
      for (let i = 0; i < COUNT; i++) href = new URL(kAbsolute).href;
      b.blackBox(href);
    });
    b.run(`URL.canParse() x${COUNT}`, () => {
      let ok;
      for (let i = 0; i < COUNT; i++) ok = URL.canParse(kRelative, kAbsolute);
      b.blackBox(ok);
    });
    b.run(`URL.parse() failing x${COUNT}`, () => {
      let url;
      for (let i = 0; i < COUNT; i++) url = URL.parse('not a url');
      b.blackBox(url);
    });
  },
};

export const attributes = {
  bench(b) {
    const url = new URL(kAbsolute);
    b.run(`every getter x${COUNT}`, () => {
      for (let i = 0; i < COUNT; i++) {
        b.blackBox(url.href);
        b.blackBox(url.origin);
        b.blackBox(url.protocol);
        b.blackBox(url.username);
        b.blackBox(url.password);
        b.blackBox(url.host);
        b.blackBox(url.hostname);
        b.blackBox(url.port);
        b.blackBox(url.pathname);
        b.blackBox(url.search);
        b.blackBox(url.searchParams);
        b.blackBox(url.hash);
      }
    });
    b.run(`origin x${COUNT}`, () => {
      for (let i = 0; i < COUNT; i++) b.blackBox(url.origin);
    });
    b.run(`pathname setter x${COUNT}`, () => {
      for (let i = 0; i < COUNT; i++) url.pathname = i & 1 ? '/p/q' : '/r/s';
    });
    b.run(`search setter x${COUNT}`, () => {
      for (let i = 0; i < COUNT; i++) url.search = i & 1 ? 'a=1' : 'b=2';
    });
    b.run(`host setter, then origin x${COUNT}`, () => {
      for (let i = 0; i < COUNT; i++) {
        url.host = i & 1 ? 'a.example.com' : 'b.example.com';
        b.blackBox(url.origin);
      }
    });
  },
};

export const searchParams = {
  bench(b) {
    const params = new URLSearchParams(kQuery);
    b.run(`new URLSearchParams(10 pairs) x${COUNT}`, () => {
      let p;
      for (let i = 0; i < COUNT; i++) p = new URLSearchParams(kQuery);
      b.blackBox(p);
    });
    b.run(`get() of 10 pairs x${COUNT}`, () => {
      for (let i = 0; i < COUNT; i++) b.blackBox(params.get('k7'));
    });
    b.run(`append() and toString() x${COUNT}`, () => {
      const p = new URLSearchParams();
      for (let i = 0; i < COUNT; i++) p.append('key', 'value with spaces');
      b.blackBox(p.toString());
    });
    b.run(`url.searchParams.append() x${COUNT}`, () => {
      const url = new URL('https://example.com/?a=1');
      for (let i = 0; i < COUNT; i++) url.searchParams.append('key', 'v');
      b.blackBox(url.href);
    });
    b.run(`iterate 10 pairs`, () => {
      for (const [k, v] of params) {
        b.blackBox(k);
        b.blackBox(v);
      }
    });
  },
};
