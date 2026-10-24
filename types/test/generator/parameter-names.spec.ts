// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  getParameterName,
  installParameterNames,
} from '../../src/generator/parameter-names';

test('parameter names prefer RTTI and retain the extracted-name fallback', () => {
  installParameterNames({
    'test::Resource': { method: ['extracted'], $constructor: ['initial'] },
  });
  assert.equal(
    getParameterName('test::Resource', 'method', 0, 'explicit'),
    'explicit'
  );
  assert.equal(
    getParameterName('test::Resource', 'method', 0, ''),
    'extracted'
  );
  assert.equal(getParameterName('test::Resource', 'constructor', 0), 'initial');
  assert.equal(getParameterName('test::Resource', 'method', 1), 'param1');
  installParameterNames({});
});

test('reserved binding identifiers are escaped in RTTI parameter names', () => {
  for (const name of [
    'new',
    'class',
    'default',
    'delete',
    'this',
    'void',
    'await',
    'yield',
    'arguments',
    'eval',
  ]) {
    assert.equal(
      getParameterName('test::Resource', 'method', 0, name),
      `$${name}`
    );
  }
});
