// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
// https://opensource.org/licenses/Apache-2.0

function expectType<T>(_value: T) {}

new DOMException();
new DOMException('message');
const error = new DOMException('message', 'AbortError');
expectType<Error>(error);
expectType<string>(error.name);
expectType<string>(error.message);
expectType<number>(error.code);
// @ts-expect-error name is read-only.
error.name = 'DataError';
// @ts-expect-error code is read-only.
error.code = 0;

expectType<number>(DOMException.ABORT_ERR);
expectType<number>(DOMException.DATA_CLONE_ERR);
// @ts-expect-error the constants are read-only.
DOMException.ABORT_ERR = 0;

expectType<typeof DOMException>(self.DOMException);

class SubError extends DOMException {
  constructor() {
    super('message', 'DataError');
  }
}
expectType<DOMException>(new SubError());
