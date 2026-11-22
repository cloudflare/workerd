// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, ok, rejects, strictEqual, throws } from 'node:assert';

// An experimental global, which the linter's environment doesn't declare.
const { AwsChunkedDecoderStream } = globalThis;

const encoder = new TextEncoder();

function concat(chunks) {
  const total = chunks.reduce((n, c) => n + c.byteLength, 0);
  const result = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    result.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return result;
}

// Encodes `data` as aws-chunked, in chunks of `chunkSize`, with the given trailer lines.
function encode(data, { chunkSize = 7, trailer = [], extension = '' } = {}) {
  const parts = [];
  for (let i = 0; i < data.byteLength; i += chunkSize) {
    const chunk = data.subarray(i, i + chunkSize);
    parts.push(
      encoder.encode(`${chunk.byteLength.toString(16)}${extension}\r\n`),
      chunk
    );
    parts.push(encoder.encode('\r\n'));
  }
  parts.push(encoder.encode(`0${extension}\r\n`));
  for (const line of trailer) parts.push(encoder.encode(`${line}\r\n`));
  parts.push(encoder.encode('\r\n'));
  return concat(parts);
}

// Writes `input` to the stream in pieces of `writeSize` bytes while reading the output.
async function run(stream, input, writeSize = input.byteLength || 1) {
  const writer = stream.writable.getWriter();
  const writing = (async () => {
    for (let i = 0; i < input.byteLength; i += writeSize) {
      await writer.write(input.slice(i, i + writeSize));
    }
    await writer.close();
  })();
  writing.catch(() => {});
  const chunks = [];
  for await (const chunk of stream.readable) chunks.push(chunk);
  await writing;
  return concat(chunks);
}

const BODY = encoder.encode('The quick brown fox jumps over the lazy dog.');
const CHECKSUM = 'x-amz-checksum-crc32:sOO8/Q==';

export const decodes = {
  async test() {
    const input = encode(BODY, { trailer: [CHECKSUM] });
    // Every split of the input into writes decodes the same way.
    for (const writeSize of [1, 2, 3, 5, 13, input.byteLength]) {
      const stream = new AwsChunkedDecoderStream();
      deepStrictEqual(await run(stream, input, writeSize), BODY);
      const trailers = await stream.trailers;
      strictEqual(trailers.get('x-amz-checksum-crc32'), 'sOO8/Q==');
      strictEqual([...trailers].length, 1);
    }
  },
};

export const noTrailers = {
  async test() {
    const stream = new AwsChunkedDecoderStream({
      decodedLength: BODY.byteLength,
    });
    deepStrictEqual(await run(stream, encode(BODY)), BODY);
    strictEqual([...(await stream.trailers)].length, 0);

    const empty = new AwsChunkedDecoderStream({ decodedLength: 0 });
    deepStrictEqual(
      await run(empty, encoder.encode('0\r\n\r\n')),
      new Uint8Array()
    );
  },
};

export const extensionsAndTrailerFields = {
  async test() {
    const stream = new AwsChunkedDecoderStream({
      allowedTrailers: ['X-Amz-Checksum-CRC32'],
    });
    const input = encode(BODY, {
      chunkSize: 16,
      extension: ';chunk-signature=' + 'a'.repeat(64),
      trailer: [
        'x-amz-checksum-crc32:  sOO8/Q==  ',
        'x-amz-trailer-signature:' + 'b'.repeat(64),
      ],
    });
    deepStrictEqual(await run(stream, input, 10), BODY);
    const trailers = await stream.trailers;
    strictEqual(trailers.get('x-amz-checksum-crc32'), 'sOO8/Q==');
    strictEqual(trailers.get('x-amz-trailer-signature'), null);
  },
};

export const hexSizes = {
  async test() {
    const data = new Uint8Array(300).fill(65);
    // Upper- and lower-case hex, with leading zeros.
    const input = concat([
      encoder.encode('00000000000000FF\r\n'),
      data.subarray(0, 255),
      encoder.encode('\r\n2d\r\n'),
      data.subarray(255),
      encoder.encode('\r\n0\r\n\r\n'),
    ]);
    deepStrictEqual(await run(new AwsChunkedDecoderStream(), input, 64), data);
  },
};

async function rejectsMalformed(input, message, options = {}) {
  const stream = new AwsChunkedDecoderStream(options);
  await rejects(run(stream, input), (e) => {
    ok(e instanceof TypeError, `${e}`);
    ok(message.test(e.message), e.message);
    return true;
  });
  await rejects(stream.trailers, TypeError);
}

export const malformed = {
  async test() {
    const good = encode(BODY, { trailer: [CHECKSUM] });
    await rejectsMalformed(
      encoder.encode('zz\r\n'),
      /Malformed aws-chunked chunk size/
    );
    await rejectsMalformed(
      encoder.encode('\r\n'),
      /Malformed aws-chunked chunk size/
    );
    await rejectsMalformed(
      encoder.encode('5\n'),
      /Malformed aws-chunked chunk size/
    );
    await rejectsMalformed(encoder.encode('5\r\rhello'), /chunk size line/);
    await rejectsMalformed(
      encoder.encode('1'.repeat(17) + '\r\n'),
      /more than 16 digits/
    );
    await rejectsMalformed(
      encoder.encode('5\r\nhelloX\r\n'),
      /longer than its declared size/
    );
    await rejectsMalformed(encoder.encode('5\r\nhello\rX'), /chunk ending/);
    await rejectsMalformed(
      good.subarray(0, 20),
      /ended before its final chunk/
    );
    await rejectsMalformed(
      good.subarray(0, good.byteLength - 1),
      /ended before its final chunk/
    );
    await rejectsMalformed(
      concat([good, encoder.encode('x')]),
      /continues past its trailer/
    );
    await rejectsMalformed(
      encode(BODY, { trailer: ['no colon here'] }),
      /Malformed aws-chunked trailer field/
    );
    await rejectsMalformed(
      encode(BODY, { trailer: ['bad name: x'] }),
      /trailer field name/
    );
    await rejectsMalformed(
      encode(BODY, { extension: ';' + 'e'.repeat(5000) }),
      /chunk extension is longer/
    );
  },
};

export const lengthsAndLimits = {
  async test() {
    const input = encode(BODY, { trailer: [CHECKSUM] });
    await rejectsMalformed(input, /decodes to more than its decoded length/, {
      decodedLength: BODY.byteLength - 1,
    });
    await rejectsMalformed(
      input,
      /decoded to 44 bytes, but its decoded length is 45/,
      {
        decodedLength: BODY.byteLength + 1,
      }
    );
    await rejectsMalformed(
      encode(BODY, { trailer: ['x-a:' + 'v'.repeat(100)] }),
      /trailer is longer than 64 bytes/,
      { maxTrailerBytes: 64 }
    );
    // Exactly at the limit is fine: the section, with its final CRLF, is 64 bytes.
    const atLimit = new AwsChunkedDecoderStream({ maxTrailerBytes: 64 });
    deepStrictEqual(
      await run(
        atLimit,
        encode(BODY, { trailer: ['x-a:' + 'v'.repeat(56)] }),
        3
      ),
      BODY
    );
    throws(
      () => new AwsChunkedDecoderStream({ decodedLength: -1 }),
      RangeError
    );
    throws(
      () => new AwsChunkedDecoderStream({ maxTrailerBytes: 0.5 }),
      RangeError
    );
  },
};

// By default, invalid input is read to its end before the stream errors, so a server can still
// respond to the client. Without draining, the write fails at once.
export const drainOnError = {
  async test() {
    const bad = concat([encoder.encode('zz\r\n'), new Uint8Array(1000)]);

    const draining = new AwsChunkedDecoderStream();
    const writer = draining.writable.getWriter();
    writer.closed.catch(() => {});
    const reading = new Response(draining.readable).arrayBuffer();
    reading.catch(() => {});
    for (let i = 0; i < bad.byteLength; i += 100) {
      await writer.write(bad.slice(i, i + 100));
    }
    const closing = writer.close();
    writer.ready.catch(() => {});
    await rejects(closing, /Malformed aws-chunked chunk size/);
    await rejects(reading, /Malformed aws-chunked chunk size/);
    await rejects(draining.trailers, /Malformed aws-chunked chunk size/);

    const strict = new AwsChunkedDecoderStream({ drainOnError: false });
    const strictWriter = strict.writable.getWriter();
    strictWriter.closed.catch(() => {});
    new Response(strict.readable).arrayBuffer().catch(() => {});
    await rejects(strictWriter.write(bad), /Malformed aws-chunked chunk size/);
    await rejects(strict.trailers, /Malformed aws-chunked chunk size/);
  },
};

// A request body piped through the decoder never needs JavaScript per chunk.
export const nativePipe = {
  async test() {
    const body = new Uint8Array(200000);
    for (let i = 0; i < body.length; i++) body[i] = i * 31;
    const input = encode(body, { chunkSize: 65536, trailer: [CHECKSUM] });
    const stream = new AwsChunkedDecoderStream({
      decodedLength: body.byteLength,
    });
    const output = new Uint8Array(
      await new Response(
        new Response(input).body.pipeThrough(stream)
      ).arrayBuffer()
    );
    deepStrictEqual(output, body);
    strictEqual(
      (await stream.trailers).get('x-amz-checksum-crc32'),
      'sOO8/Q=='
    );
  },
};

// Abandoning the stream rejects the trailers.
export const abandoned = {
  async test() {
    const stream = new AwsChunkedDecoderStream();
    const writer = stream.writable.getWriter();
    await writer.write(encoder.encode('5\r\nhel'));
    await writer.abort(new Error('client went away'));
    await rejects(stream.trailers);
  },
};
