// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { deepStrictEqual, ok, rejects, strictEqual, throws } from 'node:assert';
import { FIXTURES, SENTENCE } from './aws-chunked-sdk-fixtures.js';

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

// Compares bytes directly: deepStrictEqual compares typed arrays element by element through
// generic code, which is slow on large arrays in debug builds.
function bytesEqual(actual, expected, message) {
  ok(actual instanceof Uint8Array, message);
  strictEqual(actual.byteLength, expected.byteLength, message);
  for (let i = 0; i < actual.byteLength; i++) {
    if (actual[i] !== expected[i]) {
      strictEqual(
        actual[i],
        expected[i],
        `${message ?? 'bytes'} differ at ${i}`
      );
    }
  }
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
    bytesEqual(await run(new AwsChunkedDecoderStream(), input, 64), data);
  },
};

// Wire bytes written out by hand, rather than by encode(), in the forms S3 clients send.
const SIGNATURE =
  'ad80c730a21e5b8d04586a2213dd63b9a0e99e0e2307b0ade35a65485a288648';
const WIRE_BODY = encoder.encode('hello world');

export const wireFixtures = {
  async test() {
    // STREAMING-UNSIGNED-PAYLOAD-TRAILER, with and without the LF some clients send after the
    // checksum.
    for (const checksumLine of [
      'x-amz-checksum-crc32:DUoRhQ==\r\n',
      'x-amz-checksum-crc32:DUoRhQ==\n\r\n',
    ]) {
      const stream = new AwsChunkedDecoderStream({ decodedLength: 11 });
      const wire = encoder.encode(
        '6\r\nhello \r\n5\r\nworld\r\n0\r\n' + checksumLine + '\r\n'
      );
      deepStrictEqual(await run(stream, wire, 4), WIRE_BODY);
      strictEqual(
        (await stream.trailers).get('x-amz-checksum-crc32'),
        'DUoRhQ=='
      );
    }

    // STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER: every chunk, the final one included, carries a
    // signature, and the trailer has its own.
    const signed = new AwsChunkedDecoderStream({
      decodedLength: 11,
      allowedTrailers: ['x-amz-checksum-crc32'],
    });
    const signedWire = encoder.encode(
      `6;chunk-signature=${SIGNATURE}\r\nhello \r\n` +
        `5;chunk-signature=${SIGNATURE}\r\nworld\r\n` +
        `0;chunk-signature=${SIGNATURE}\r\n` +
        'x-amz-checksum-crc32:DUoRhQ==\n\r\n' +
        `x-amz-trailer-signature:${SIGNATURE}\r\n` +
        '\r\n'
    );
    deepStrictEqual(await run(signed, signedWire, 7), WIRE_BODY);
    const trailers = await signed.trailers;
    strictEqual(trailers.get('x-amz-checksum-crc32'), 'DUoRhQ==');
    strictEqual([...trailers].length, 1);

    // STREAMING-AWS4-HMAC-SHA256-PAYLOAD, with no trailer.
    const plain = new AwsChunkedDecoderStream({ decodedLength: 11 });
    const plainWire = encoder.encode(
      `b;chunk-signature=${SIGNATURE}\r\nhello world\r\n` +
        `0;chunk-signature=${SIGNATURE}\r\n\r\n`
    );
    deepStrictEqual(await run(plain, plainWire, 5), WIRE_BODY);
    strictEqual([...(await plain.trailers)].length, 0);
  },
};

// Bodies that S3 clients sent: see aws-chunked-fixtures/README.md.
const DIGESTS = {
  'x-amz-checksum-crc32': 'crc32',
  'x-amz-checksum-crc32c': 'crc32c',
  'x-amz-checksum-crc64nvme': 'crc64nvme',
  'x-amz-checksum-sha1': 'SHA-1',
  'x-amz-checksum-sha256': 'SHA-256',
};

function fixturePayload(length) {
  return encoder.encode(
    SENTENCE.repeat(Math.ceil(length / SENTENCE.length)).slice(0, length)
  );
}

function fixtureBody(fixture) {
  const payload = fixturePayload(fixture.decodedLength);
  let offset = 0;
  const parts = fixture.body.map((part) => {
    if (typeof part === 'string') return encoder.encode(part);
    offset += part;
    return payload.subarray(offset - part, offset);
  });
  return concat(parts);
}

async function checksum(trailer, data) {
  const digest = new crypto.DigestStream(DIGESTS[trailer]);
  const writer = digest.getWriter();
  await writer.write(data);
  await writer.close();
  return btoa(String.fromCharCode(...new Uint8Array(await digest.digest)));
}

export const sdkFixtures = {
  async test() {
    for (const fixture of FIXTURES) {
      const input = fixtureBody(fixture);
      const payload = fixturePayload(fixture.decodedLength);
      const expectedTrailers = fixture.trailer
        ? [[fixture.trailer, await checksum(fixture.trailer, payload)]]
        : [];
      for (const writeSize of [input.byteLength || 1, 65539, 1000]) {
        const where = `${fixture.name}, writes of ${writeSize}`;
        const stream = new AwsChunkedDecoderStream({
          decodedLength: fixture.decodedLength,
          ...(fixture.trailer && { allowedTrailers: [fixture.trailer] }),
        });
        bytesEqual(await run(stream, input, writeSize), payload, where);
        // The decoded bytes are what the client checksummed.
        deepStrictEqual([...(await stream.trailers)], expectedTrailers, where);
      }

      // With no filter, a signed trailer's signature is kept too.
      const unfiltered = new AwsChunkedDecoderStream();
      await run(unfiltered, input);
      const names = [...(await unfiltered.trailers)].map(([name]) => name);
      const expected = fixture.trailer ? [fixture.trailer] : [];
      if (
        fixture.contentSha256 === 'STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER'
      ) {
        expected.push('x-amz-trailer-signature');
      }
      deepStrictEqual(names, expected, fixture.name);
    }
  },
};

// A kept trailer value is returned unchanged, so it must be UTF-8; a dropped one need not be.
export const trailerValueEncoding = {
  async test() {
    const kept = new AwsChunkedDecoderStream();
    await run(kept, encoder.encode('0\r\nx-a: caf\u00e9 \u{1f600}\r\n\r\n'));
    strictEqual((await kept.trailers).get('x-a'), 'caf\u00e9 \u{1f600}');

    const latin1 = concat([
      encoder.encode('0\r\nx-a: caf'),
      new Uint8Array([0xe9]),
      encoder.encode('\r\n' + CHECKSUM + '\r\n\r\n'),
    ]);
    const rejected = new AwsChunkedDecoderStream();
    await rejects(run(rejected, latin1), {
      name: 'TypeError',
      message: 'aws-chunked trailer field value is not UTF-8.',
    });
    await rejects(rejected.trailers, { name: 'TypeError' });

    const dropped = new AwsChunkedDecoderStream({
      allowedTrailers: ['x-amz-checksum-crc32'],
    });
    await run(dropped, latin1);
    deepStrictEqual(
      [...(await dropped.trailers)],
      [['x-amz-checksum-crc32', 'sOO8/Q==']]
    );
  },
};

// Chunk extensions follow RFC 9112: whitespace around ";" and "=", names without values, and
// quoted values with escapes.
export const extensionGrammar = {
  async test() {
    for (const extension of [
      ';a',
      ';a=b',
      ';a;b=c',
      ' ;a',
      '\t; a = b ;c',
      ';a="quoted ; value"',
      ';a="escaped \\" quote"',
      ';a=""',
      ';a = "x"\t; b',
    ]) {
      const stream = new AwsChunkedDecoderStream();
      deepStrictEqual(
        await run(stream, encode(BODY, { extension }), 3),
        BODY,
        JSON.stringify(extension)
      );
    }
    for (const extension of [
      ';',
      ';;a',
      ';a=',
      ';a b',
      ';a=b c',
      ';a="unterminated',
      ';a="x"y',
      ';a=b ',
      ' ',
      ';a ',
      ';@',
      ';a=@',
      ';a="\x01"',
    ]) {
      await rejectsMalformed(
        encode(BODY, { extension }),
        /Malformed aws-chunked chunk extension/
      );
    }
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

// An error that the end of the input reveals reaches a consumer that reads the whole body, even
// when the decoded length says the body is complete without it, and whether the stream had
// failed before the consumer started reading or fails while it waits.
export const lateFailureWholeBody = {
  async test() {
    const cases = [
      [
        { decodedLength: BODY.byteLength },
        encode(BODY, { trailer: ['bad name: x'] }),
        /trailer field name/,
      ],
      [
        { decodedLength: BODY.byteLength, drainOnError: false },
        encode(BODY, { trailer: ['bad name: x'] }),
        /trailer field name/,
      ],
      [
        { decodedLength: BODY.byteLength },
        encode(BODY).subarray(0, encode(BODY).byteLength - 1),
        /ended before its final chunk/,
      ],
      [{ decodedLength: 0 }, encoder.encode('0\r\n'), /ended before/],
      [{ decodedLength: 0 }, encoder.encode('0\r\nx\r\n'), /ended before/],
    ];
    for (const [options, input, message] of cases) {
      // Failed before reading began.
      const failed = new AwsChunkedDecoderStream(options);
      const writer = failed.writable.getWriter();
      const writing = writer.write(input).then(() => writer.close());
      await rejects(writing, message);
      await rejects(new Response(failed.readable).arrayBuffer(), message);

      // Failing while a read waits.
      const pending = new AwsChunkedDecoderStream(options);
      const body = new Response(pending.readable).arrayBuffer();
      body.catch(() => {});
      const pendingWriter = pending.writable.getWriter();
      await rejects(
        pendingWriter.write(input).then(() => pendingWriter.close()),
        message
      );
      await rejects(body, message);
    }

    // The same lengths decode when the input is intact.
    const intact = new AwsChunkedDecoderStream({
      decodedLength: BODY.byteLength,
    });
    const writer = intact.writable.getWriter();
    const writing = writer
      .write(encode(BODY, { trailer: [CHECKSUM] }))
      .then(() => writer.close());
    deepStrictEqual(
      new Uint8Array(await new Response(intact.readable).arrayBuffer()),
      BODY
    );
    await writing;
    const empty = new AwsChunkedDecoderStream({ decodedLength: 0 });
    const emptyWriter = empty.writable.getWriter();
    const emptyWriting = emptyWriter
      .write(encoder.encode('0\r\n\r\n'))
      .then(() => emptyWriter.close());
    strictEqual(
      (await new Response(empty.readable).arrayBuffer()).byteLength,
      0
    );
    await emptyWriting;
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
    bytesEqual(output, body);
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
