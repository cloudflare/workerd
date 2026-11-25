// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';

function expectType<T>(_value: T) {}

new DOMException();
new DOMException('message');
const error = new DOMException('message', 'AbortError');
expectType<Error>(error);
expectType<string>(error.name);
expectType<string>(error.message);
expectType<number>(error.code);
// stack is Error's.
expectType<string | undefined>(error.stack);
// @ts-expect-error stack may be undefined.
error.stack.length;
// @ts-expect-error name is read-only.
error.name = 'DataError';
// @ts-expect-error code is read-only.
error.code = 0;

// The constants have their codes' literal types, on the constructor and on
// instances.
expectType<20>(DOMException.ABORT_ERR);
expectType<25>(DOMException.DATA_CLONE_ERR);
expectType<20>(error.ABORT_ERR);
// @ts-expect-error the constants are read-only.
DOMException.ABORT_ERR = 0;
// @ts-expect-error the constants are read-only.
error.ABORT_ERR = 20;

expectType<typeof DOMException>(self.DOMException);

// The constructor's prototype is typed as in lib.dom.d.ts, not read-only, so
// that a program with both declarations of DOMException compiles: a variable's
// declarations must agree on its type (TS2403).
expectTypeOf<Pick<typeof DOMException, 'prototype'>>().toEqualTypeOf<{
  prototype: DOMException;
}>();

const caught: unknown = error;
if (caught instanceof DOMException) {
  expectType<string>(caught.name);
}

class SubError extends DOMException {
  constructor() {
    super('message', 'DataError');
  }
}
expectType<DOMException>(new SubError());
