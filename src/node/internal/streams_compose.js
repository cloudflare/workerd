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

import { pipeline } from 'node-internal:streams_pipeline';
import { Duplex } from 'node-internal:streams_duplex';
import { Readable as ReadableConstructor } from 'node-internal:streams_readable';
import {
  isNodeStream,
  isReadable,
  isWritable,
  isWebStream,
  isTransformStream,
  isWritableStream,
  isReadableStream,
  kIsClosedPromise,
} from 'node-internal:streams_util';
import { destroyer } from 'node-internal:streams_destroy';
import { eos } from 'node-internal:streams_end_of_stream';
import {
  AbortError,
  ERR_INVALID_ARG_VALUE,
  ERR_MISSING_ARGS,
  ERR_WEB_STREAM_INTEROP_UNSUPPORTED,
} from 'node-internal:internal_errors';

export function compose(...streams) {
  if (streams.length === 0) {
    throw new ERR_MISSING_ARGS('streams');
  }

  if (streams.length === 1) {
    return Duplex.from(streams[0]);
  }

  const orgStreams = [...streams];

  if (typeof streams[0] === 'function') {
    streams[0] = Duplex.from(streams[0]);
  }

  if (typeof streams[streams.length - 1] === 'function') {
    const idx = streams.length - 1;
    streams[idx] = Duplex.from(streams[idx]);
  }

  for (let n = 0; n < streams.length; ++n) {
    if (!isNodeStream(streams[n]) && !isWebStream(streams[n])) {
      // TODO(ronag): Add checks for non streams.
      continue;
    }
    if (
      n < streams.length - 1 &&
      !(
        isReadable(streams[n]) ||
        isReadableStream(streams[n]) ||
        isTransformStream(streams[n])
      )
    ) {
      throw new ERR_INVALID_ARG_VALUE(
        `streams[${n}]`,
        orgStreams[n],
        'must be readable'
      );
    }
    if (
      n > 0 &&
      !(
        isWritable(streams[n]) ||
        isWritableStream(streams[n]) ||
        isTransformStream(streams[n])
      )
    ) {
      throw new ERR_INVALID_ARG_VALUE(
        `streams[${n}]`,
        orgStreams[n],
        'must be writable'
      );
    }
  }

  let ondrain;
  let onfinish;
  let onreadable;
  let onclose;
  let d;

  function onfinished(err) {
    const cb = onclose;
    onclose = null;

    if (cb) {
      cb(err);
    } else if (err) {
      d.destroy(err);
    } else if (!readable && !writable) {
      d.destroy();
    }
  }

  const head = streams[0];
  const last = streams[streams.length - 1];

  const writable = !!(
    isWritable(head) ||
    isWritableStream(head) ||
    isTransformStream(head)
  );

  // A composed stream with a writable side must learn when its tail
  // finishes; for a web tail that takes the Node.js interop hook (see
  // streams_end_of_stream.ts). Refused before the pipeline starts and before
  // any lock is taken, so the caller's streams are left as they were.
  if (writable && isWebStream(last)) {
    const toRead = isTransformStream(last) ? last.readable : last;
    if (toRead[kIsClosedPromise] === undefined) {
      throw new ERR_WEB_STREAM_INTEROP_UNSUPPORTED('compose()');
    }
  }

  const tail = pipeline(streams, onfinished);

  const readable = !!(
    isReadable(tail) ||
    isReadableStream(tail) ||
    isTransformStream(tail)
  );

  // TODO(ronag): Avoid double buffering.
  // Implement Writable/Readable/Duplex traits.
  // See, https://github.com/nodejs/node/pull/33515.
  d = new Duplex({
    // TODO (ronag): highWaterMark?
    writableObjectMode: !!head?.writableObjectMode,
    readableObjectMode: !!tail?.readableObjectMode,
    writable,
    readable,
  });

  if (writable) {
    if (isNodeStream(head)) {
      d._write = function (chunk, encoding, callback) {
        if (head.write(chunk, encoding)) {
          callback();
        } else {
          ondrain = callback;
        }
      };

      d._final = function (callback) {
        head.end();
        onfinish = callback;
      };

      head.on('drain', function () {
        if (ondrain) {
          const cb = ondrain;
          ondrain = null;
          cb();
        }
      });
    } else if (isWebStream(head)) {
      const writable = isTransformStream(head) ? head.writable : head;
      const writer = writable.getWriter();

      d._write = async function (chunk, encoding, callback) {
        try {
          await writer.ready;
          writer.write(chunk).catch(() => {});
          callback();
        } catch (err) {
          callback(err);
        }
      };

      d._final = async function (callback) {
        try {
          await writer.ready;
          writer.close().catch(() => {});
          onfinish = callback;
        } catch (err) {
          callback(err);
        }
      };
    }

    const toRead = isTransformStream(tail) ? tail.readable : tail;

    eos(toRead, () => {
      if (onfinish) {
        const cb = onfinish;
        onfinish = null;
        cb();
      }
    });
  }

  if (readable) {
    if (isNodeStream(tail)) {
      tail.on('readable', function () {
        if (onreadable) {
          const cb = onreadable;
          onreadable = null;
          cb();
        }
      });

      tail.on('end', function () {
        d.push(null);
      });

      d._read = function () {
        while (true) {
          const buf = tail.read();
          if (buf === null) {
            onreadable = d._read;
            return;
          }

          if (!d.push(buf)) {
            return;
          }
        }
      };
    } else if (isWebStream(tail)) {
      const readable = isTransformStream(tail) ? tail.readable : tail;
      const reader = readable.getReader();
      d._read = async function () {
        while (true) {
          try {
            const { value, done } = await reader.read();

            if (!d.push(value)) {
              return;
            }

            if (done) {
              d.push(null);
              return;
            }
          } catch {
            return;
          }
        }
      };
    }
  }

  d._destroy = function (err, callback) {
    if (!err && onclose !== null) {
      err = new AbortError();
    }

    onreadable = null;
    ondrain = null;
    onfinish = null;

    if (isNodeStream(tail)) {
      destroyer(tail, err);
    }

    if (onclose === null) {
      callback(err);
    } else {
      onclose = callback;
    }
  };

  return d;
}

ReadableConstructor.prototype.compose = function (...streams) {
  return compose(this, ...streams);
};
