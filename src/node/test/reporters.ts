// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ERR_METHOD_NOT_IMPLEMENTED } from 'node-internal:internal_errors';

// Each reporter is an async generator function that consumes test events and
// yields formatted output. Workerd has no test runner, so all reporters throw.
// The reporter functions are declared async generators to match the Node.js
// shape (they're identified by name and `.constructor.name === 'AsyncGeneratorFunction'`).

// eslint-disable-next-line @typescript-eslint/no-unused-vars
export async function* tap(
  _source: AsyncIterable<unknown>
): AsyncGenerator<string> {
  throw new ERR_METHOD_NOT_IMPLEMENTED('tap reporter');
}

// eslint-disable-next-line @typescript-eslint/no-unused-vars
export async function* spec(
  _source: AsyncIterable<unknown>
): AsyncGenerator<string> {
  throw new ERR_METHOD_NOT_IMPLEMENTED('spec reporter');
}

// eslint-disable-next-line @typescript-eslint/no-unused-vars
export async function* dot(
  _source: AsyncIterable<unknown>
): AsyncGenerator<string> {
  throw new ERR_METHOD_NOT_IMPLEMENTED('dot reporter');
}

// eslint-disable-next-line @typescript-eslint/no-unused-vars
export async function* junit(
  _source: AsyncIterable<unknown>
): AsyncGenerator<string> {
  throw new ERR_METHOD_NOT_IMPLEMENTED('junit reporter');
}

// eslint-disable-next-line @typescript-eslint/no-unused-vars
export async function* lcov(
  _source: AsyncIterable<unknown>
): AsyncGenerator<string> {
  throw new ERR_METHOD_NOT_IMPLEMENTED('lcov reporter');
}

export default {
  tap,
  spec,
  dot,
  junit,
  lcov,
};
