// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// util.inspect() of a URLPattern and its prototype. Both implementations
// format through formatJsgResourceType, which lists the prototype's
// accessors in order.

import { inspect } from 'node:util';
import { strictEqual } from 'node:assert';
import { urlPatternStandard, setToStringTag } from 'which-impl';
import { kAttributes } from 'helpers';

export const inspectPattern = {
  test() {
    const values = {
      protocol: "'https'",
      username: "'*'",
      password: "'*'",
      hostname: "'a.com'",
      port: "''",
      pathname: "'/b/:c'",
      search: "'*'",
      hash: "'*'",
      hasRegExpGroups: 'false',
    };
    const pattern = new URLPattern({
      protocol: 'https',
      hostname: 'a.com',
      port: '',
      pathname: '/b/:c',
    });
    const lines = kAttributes.map((k) => `  ${k}: ${values[k]}`);
    strictEqual(inspect(pattern), `URLPattern {\n${lines.join(',\n')}\n}`);
  },
};

export const inspectPrototype = {
  test() {
    const lines = [
      ...kAttributes.map((k) => `  ${k}: [Getter]`),
      '  test: [Function: test]',
      '  exec: [Function: exec]',
    ];
    const prefix = setToStringTag ? 'Object [URLPattern] ' : '';
    strictEqual(
      inspect(URLPattern.prototype),
      `${prefix}{\n${lines.join(',\n')}\n}`
    );
    strictEqual(urlPatternStandard, kAttributes.includes('hasRegExpGroups'));
  },
};
