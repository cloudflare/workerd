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

/* TODO: the following is adopted code, enabling linting one day */
/* eslint-disable */

import { pipeline } from 'node-internal:streams_pipeline';
import { Duplex } from 'node-internal:streams_duplex';
import {
  Readable as ReadableConstructor,
  from,
} from 'node-internal:streams_readable';
import {
  isNodeStream,
  isReadable,
  isWritable,
  destroyer,
} from 'node-internal:streams_util';
import {
  AbortError,
  ERR_INVALID_ARG_VALUE,
  ERR_MISSING_ARGS,
} from 'node-internal:internal_errors';

export function compose(...streams) {
  if (streams.length === 0) {
    throw new ERR_MISSING_ARGS('streams');
  }
  if (streams.length === 1) {
    return from(Duplex, streams[0]);
  }
  const orgStreams = [...streams];
  if (typeof streams[0] === 'function') {
    streams[0] = from(Duplex, streams[0]);
  }
  if (typeof streams[streams.length - 1] === 'function') {
    const idx = streams.length - 1;
    streams[idx] = from(Duplex, streams[idx]);
  }
  for (let n = 0; n < streams.length; ++n) {
    if (!isNodeStream(streams[n])) {
      // TODO(ronag): Add checks for non streams.
      continue;
    }
    if (n < streams.length - 1 && !isReadable(streams[n])) {
      throw new ERR_INVALID_ARG_VALUE(
        `streams[${n}]`,
        orgStreams[n],
        'must be readable'
      );
    }
    if (n > 0 && !isWritable(streams[n])) {
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
  const tail = pipeline(streams, onfinished);
  const writable = !!isWritable(head);
  const readable = !!isReadable(tail);

  // TODO(ronag): Avoid double buffering.
  // Implement Writable/Readable/Duplex traits.
  // See, https://github.com/nodejs/node/pull/33515.
  d = new Duplex({
    // TODO (ronag): highWaterMark?
    writableObjectMode: !!(
      head !== null &&
      head !== undefined &&
      head.writableObjectMode
    ),
    readableObjectMode: !!(
      tail !== null &&
      tail !== undefined &&
      tail.writableObjectMode
    ),
    writable,
    readable,
  });
  if (writable) {
    const w = head;
    d._write = function (chunk, encoding, callback) {
      if (head.write(chunk, encoding)) {
        callback();
      } else {
        ondrain = callback;
      }
    };
    d._final = function (callback) {
      w.end();
      onfinish = callback;
    };
    w.on('drain', function () {
      if (ondrain) {
        const cb = ondrain;
        ondrain = null;
        cb();
      }
    });
    tail.on('finish', function () {
      if (onfinish) {
        const cb = onfinish;
        onfinish = null;
        cb();
      }
    });
  }
  if (readable) {
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
    d._read = function (_n) {
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
  }
  d._destroy = function (err, callback) {
    if (!err && onclose !== null) {
      err = new AbortError();
    }
    onreadable = null;
    ondrain = null;
    onfinish = null;
    if (onclose === null) {
      callback(err);
    } else {
      onclose = callback;
      destroyer(tail, err);
    }
  };
  return d;
}

ReadableConstructor.prototype.compose = function (...streams) {
  return compose(this, ...streams);
};
