// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// URLPattern on the standard (ada) path: construction, and matching with
// test() and exec() the way a router does. Each call does COUNT
// operations, since one costs about as much as the runner's overhead per
// call.

const COUNT = 16;

const kUrl = 'https://api.example.com/v1/users/12345/posts?sort=new#top';
const kInit = {
  hostname: 'api.example.com',
  pathname: '/v1/users/12345/posts',
};

// A small router's routes, each a pathname pattern.
const kRoutes = [
  '/v1/health',
  '/v1/users',
  '/v1/users/:id',
  '/v1/users/:id/posts',
  '/v1/users/:id/posts/:post',
  '/v1/teams/:team/*',
  '/v1/files/:path+',
  '/v1/search{/:query}?',
];

export const construction = {
  bench(b) {
    b.run(`new URLPattern(init) x${COUNT}`, () => {
      let pattern;
      for (let i = 0; i < COUNT; i++) {
        pattern = new URLPattern({ pathname: '/v1/users/:id/posts' });
      }
      b.blackBox(pattern);
    });
    b.run(`new URLPattern(string) x${COUNT}`, () => {
      let pattern;
      for (let i = 0; i < COUNT; i++) {
        pattern = new URLPattern('https://*.example.com/v1/users/:id/*');
      }
      b.blackBox(pattern);
    });
    b.run(`new URLPattern(string, base) x${COUNT}`, () => {
      let pattern;
      for (let i = 0; i < COUNT; i++) {
        pattern = new URLPattern('/v1/users/:id', 'https://example.com');
      }
      b.blackBox(pattern);
    });
  },
};

export const matching = {
  bench(b) {
    const pattern = new URLPattern({ pathname: '/v1/users/:id/posts' });
    const host = new URLPattern({
      hostname: '{*.}?example.com',
      pathname: '/v1/users/:id/*',
    });
    b.run(`test(url) x${COUNT}`, () => {
      let ok;
      for (let i = 0; i < COUNT; i++) ok = pattern.test(kUrl);
      b.blackBox(ok);
    });
    b.run(`test(url) miss x${COUNT}`, () => {
      let ok;
      for (let i = 0; i < COUNT; i++) {
        ok = pattern.test('https://api.example.com/v2/x');
      }
      b.blackBox(ok);
    });
    b.run(`test(init) x${COUNT}`, () => {
      let ok;
      for (let i = 0; i < COUNT; i++) ok = pattern.test(kInit);
      b.blackBox(ok);
    });
    b.run(`exec(url) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = pattern.exec(kUrl);
      b.blackBox(result);
    });
    b.run(`exec(url).pathname.groups.id x${COUNT}`, () => {
      let id;
      for (let i = 0; i < COUNT; i++)
        id = pattern.exec(kUrl).pathname.groups.id;
      b.blackBox(id);
    });
    b.run(`exec(init) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = pattern.exec(kInit);
      b.blackBox(result);
    });
    b.run(`two regexp components exec(url) x${COUNT}`, () => {
      let result;
      for (let i = 0; i < COUNT; i++) result = host.exec(kUrl);
      b.blackBox(result);
    });
  },
};

export const routing = {
  bench(b) {
    const routes = kRoutes.map((pathname) => new URLPattern({ pathname }));
    // The fourth route matches, after three misses.
    b.run(`route among ${kRoutes.length} x${COUNT}`, () => {
      let match;
      for (let i = 0; i < COUNT; i++) {
        match = null;
        for (const route of routes) {
          match = route.exec(kUrl);
          if (match !== null) break;
        }
      }
      b.blackBox(match);
    });
  },
};
