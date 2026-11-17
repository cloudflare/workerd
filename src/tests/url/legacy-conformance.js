// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The legacy parser against URL test data: workerd's own cases and the
// WPT cases kj::Url passes. The standard parser runs the full WPT data
// (src/wpt/url-test.ts).

import { throws, strictEqual } from 'node:assert';
import { urlStandard } from 'which-impl';
import {
  kConstructorCases,
  kSetterCases,
  kWptConstructorCases,
  kWptSetterCases,
} from 'legacy-conformance-data';

const kConstructorAttributes = [
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
];

function checkSetterCases(cases) {
  for (const [attribute, list] of Object.entries(cases)) {
    for (const c of list) {
      const url = new URL(c.href);
      url[attribute] = c.new_value;
      const name = `<${c.href}>.${attribute} = '${c.new_value}'`;
      for (const [key, value] of Object.entries(c.expected)) {
        strictEqual(url[key], value, `${name}: ${key}`);
      }
    }
  }
}

export const legacyConstructorCases = {
  test() {
    if (urlStandard) return;
    for (const c of kConstructorCases) {
      const url = new URL(c.input);
      for (const attribute of ['pathname', 'search', 'hash', 'href']) {
        if (attribute in c) {
          strictEqual(url[attribute], c[attribute], `${c.input}: ${c.comment}`);
        }
      }
    }
  },
};

export const legacySetterCases = {
  test() {
    if (urlStandard) return;
    checkSetterCases(kSetterCases);
  },
};

export const legacyWptConstructorCases = {
  test() {
    if (urlStandard) return;
    for (const c of kWptConstructorCases) {
      const base = c.base || 'about:blank';
      const name = `<${c.input}> against <${base}>`;
      if (c.failure) {
        throws(() => new URL(c.input, base), { name: 'TypeError' }, name);
        continue;
      }
      const url = new URL(c.input, base);
      for (const attribute of kConstructorAttributes) {
        strictEqual(url[attribute], c[attribute], `${name}: ${attribute}`);
      }
      if ('searchParams' in c) {
        strictEqual(url.searchParams.toString(), c.searchParams, name);
      }
      if ('origin' in c) {
        strictEqual(url.origin, c.origin, `${name}: origin`);
      }
    }
  },
};

export const legacyWptSetterCases = {
  test() {
    if (urlStandard) return;
    checkSetterCases(kWptSetterCases);
  },
};
