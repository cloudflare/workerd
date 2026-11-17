// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// util.inspect() of URL, URLSearchParams, their prototypes and iterators.
// Both implementations format through formatJsgResourceType, which lists
// own properties and then the prototype's accessors, in order (ledger #5).

import { inspect } from 'node:util';
import { strictEqual } from 'node:assert';
import {
  usingTsImpl,
  urlStandard,
  gettersSettersOnPrototype,
  setToStringTag,
} from 'which-impl';

const instanceLayout = !urlStandard && !gettersSettersOnPrototype;

const kValues = {
  href: "'http://u:p@a:81/p?q=1#h'",
  origin: "'http://a:81'",
  protocol: "'http:'",
  username: "'u'",
  password: "'p'",
  host: "'a:81'",
  hostname: "'a'",
  port: "'81'",
  pathname: "'/p'",
  search: "'?q=1'",
  searchParams: "URLSearchParams(1) { 'q' => '1' }",
  hash: "'#h'",
};

const kWebIdlOrder = Object.keys(kValues);

function urlOrder() {
  if (instanceLayout) return [...kWebIdlOrder].reverse();
  if (urlStandard && !usingTsImpl) {
    return [
      'origin',
      'href',
      'protocol',
      'username',
      'password',
      'host',
      'hostname',
      'port',
      'pathname',
      'search',
      'hash',
      'searchParams',
    ];
  }
  return kWebIdlOrder;
}

export const inspectUrl = {
  test() {
    const lines = urlOrder().map((k) => `  ${k}: ${kValues[k]}`);
    strictEqual(
      inspect(new URL('http://u:p@a:81/p?q=1#h')),
      `URL {\n${lines.join(',\n')}\n}`
    );
  },
};

export const inspectSearchParams = {
  test() {
    strictEqual(
      inspect(new URLSearchParams('a=1&b=2&a=3')),
      "URLSearchParams(3) { 'a' => '1', 'b' => '2', 'a' => '3' }"
    );
    strictEqual(inspect(new URLSearchParams()), 'URLSearchParams(0) {}');
  },
};

export const inspectIterator = {
  test() {
    const it = new URLSearchParams('a=1').entries();
    if (usingTsImpl) {
      strictEqual(
        inspect(it),
        setToStringTag
          ? 'Object [URLSearchParams Iterator] {}'
          : 'Object [Iterator] {}'
      );
    } else {
      strictEqual(inspect(it), 'EntryIterator {}');
    }
  },
};

export const inspectPrototype = {
  test() {
    const out = inspect(URL.prototype);
    const prefix = setToStringTag ? 'Object [URL] ' : '';
    if (instanceLayout && !usingTsImpl) {
      strictEqual(
        out,
        `${prefix}{ toString: [Function: toString], toJSON: [Function: toJSON] }`
      );
      return;
    }
    strictEqual(out.startsWith(`${prefix}{\n`), true, out);
    const order = usingTsImpl ? kWebIdlOrder : urlOrder();
    const indices = order.map((k) => out.indexOf(`  ${k}: [Getter`));
    strictEqual(
      indices.every((i, n) => i > 0 && (n === 0 || i > indices[n - 1])),
      true,
      out
    );
  },
};
