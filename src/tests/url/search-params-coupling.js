// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A URL and its searchParams stay in step both ways: the href and search
// setters reach an existing searchParams, and every searchParams mutator
// runs the URL's update steps, even one that changes nothing.

import { strictEqual, ok } from 'node:assert';
import { urlStandard } from 'which-impl';

export const searchParamsIsTheSameObject = {
  test() {
    const url = new URL('http://a/?a=1');
    const params = url.searchParams;
    strictEqual(url.searchParams, params);
    url.search = 'b=2';
    strictEqual(url.searchParams, params);
    url.href = 'http://b/?c=3';
    strictEqual(url.searchParams, params);
  },
};

export const urlSettersReachSearchParams = {
  test() {
    const url = new URL('http://a/?a=1');
    const params = url.searchParams;
    url.search = 'b=2';
    strictEqual(params.toString(), 'b=2');
    url.href = 'http://z/?z=9';
    strictEqual(params.toString(), 'z=9');
    url.search = '';
    strictEqual(params.size, 0);
    url.href = 'http://z/?b';
    strictEqual(params.toString(), 'b=');
  },
};

export const searchParamsMutatorsReachUrl = {
  test() {
    const url = new URL('http://a/?b=2#h');
    const params = url.searchParams;
    params.append('c', '3 4');
    strictEqual(url.href, 'http://a/?b=2&c=3+4#h');
    params.set('b', '5');
    strictEqual(url.search, '?b=5&c=3+4');
    params.sort();
    strictEqual(url.search, '?b=5&c=3+4');
    params.delete('b');
    params.delete('c');
    strictEqual(url.href, 'http://a/#h');
    strictEqual(url.search, '');
  },
};

export const noOpMutatorsStillSerialize = {
  test() {
    for (const mutate of [(p) => p.delete('nope'), (p) => p.sort()]) {
      const url = new URL('http://a/?a=b%20c');
      mutate(url.searchParams);
      strictEqual(url.href, 'http://a/?a=b+c');
    }
    const empty = new URL('http://a/?');
    empty.searchParams.sort();
    strictEqual(empty.href, 'http://a/');
  },
};

export const searchParamsCopyIsIndependent = {
  test() {
    const url = new URL('http://a/?a=1');
    const copy = new URLSearchParams(url.searchParams);
    copy.append('b', '2');
    strictEqual(url.href, 'http://a/?a=1');
    url.search = 'c=3';
    strictEqual(copy.toString(), 'a=1&b=2');
  },
};

// The legacy backend's search getter re-encodes the decoded query, while
// the standard one returns what was parsed.
export const searchGetterEncoding = {
  test() {
    const url = new URL('http://a/?a=b%20c&d=e+f&g');
    strictEqual(
      url.search,
      urlStandard ? '?a=b%20c&d=e+f&g' : '?a=b+c&d=e+f&g'
    );
    strictEqual(url.searchParams.toString(), 'a=b+c&d=e+f&g=');
    ok(url.searchParams.has('g'));
  },
};
