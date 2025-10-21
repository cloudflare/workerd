// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import { validateAbortSignal } from 'node-internal:validators';
import { isNodeStream } from 'node-internal:streams_util';
import { eos } from 'node-internal:streams_end_of_stream';
import {
  AbortError,
  ERR_INVALID_ARG_TYPE,
} from 'node-internal:internal_errors';
import { addAbortListener } from 'node-internal:events';

import type { Readable } from 'node-internal:streams_readable';
import type { Writable } from 'node-internal:streams_writable';
import type { Transform } from 'node-internal:streams_transform';

type NodeStream = Readable | Writable | Transform;

export function addAbortSignal<T extends { destroy: (err: Error) => void }>(
  signal: unknown,
  stream: T
): T {
  validateAbortSignal(signal, 'signal');
  if (!isNodeStream(stream)) {
    throw new ERR_INVALID_ARG_TYPE('stream', 'stream.Stream', stream);
  }
  const onAbort = (): void => {
    stream.destroy(
      new AbortError(undefined, {
        cause: signal.reason,
      })
    );
  };
  if (signal.aborted) {
    onAbort();
  } else {
    const disposable = addAbortListener(signal, onAbort);
    eos(stream as NodeStream, disposable[Symbol.dispose]);
  }
  return stream;
}
