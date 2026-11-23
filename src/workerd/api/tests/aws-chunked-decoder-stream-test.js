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

// Encodes `data` as unsigned aws-chunked, in chunks of `chunkSize`, with the given trailer lines.
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
      const stream = new AwsChunkedDecoderStream({
        decodedLength: BODY.byteLength,
      });
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

    // The body may end right after its final chunk.
    const ended = new AwsChunkedDecoderStream({ decodedLength: 0 });
    deepStrictEqual(
      await run(ended, encoder.encode('0\r\n')),
      new Uint8Array()
    );
    strictEqual([...(await ended.trailers)].length, 0);
  },
};

export const trailerFields = {
  async test() {
    const stream = new AwsChunkedDecoderStream({
      decodedLength: BODY.byteLength,
      allowedTrailers: ['X-Amz-Checksum-CRC32'],
    });
    const input = encode(BODY, {
      chunkSize: 16,
      trailer: ['X-Amz-Checksum-CRC32:  sOO8/Q==  ', 'x-amz-other:b'],
    });
    deepStrictEqual(await run(stream, input, 10), BODY);
    const trailers = await stream.trailers;
    deepStrictEqual([...trailers], [['x-amz-checksum-crc32', 'sOO8/Q==']]);
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
    deepStrictEqual(
      await run(new AwsChunkedDecoderStream({ decodedLength: 300 }), input, 64),
      data
    );
  },
};

// The example in AWS's "Signature Calculations for the Authorization Header: Transferring
// Payload in Multiple Chunks": 66560 bytes of "a", sent as a 64 KiB chunk and a 1 KiB chunk.
const SIGNING = {
  secretAccessKey: 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY',
  amzDate: '20130524T000000Z',
  region: 'us-east-1',
  service: 's3',
  seedSignature:
    '4f232c4386841ef735655705268965c44a0e4690baa4adea153f7db9fa80a0a9',
};
const EXAMPLE_SIGNATURES = [
  'ad80c730a21e5b8d04586a2213dd63b9a0e99e0e2307b0ade35a65485a288648',
  '0055627c9e194cb4542bae2aa5492e3c1575bbb81b612b7d234b86a503ef5497',
  'b6c6ea8a5354eaf15b3cb7646744f4275b71ea724fed81ceb9323e279d449df9',
];
const EXAMPLE_BODY = new Uint8Array(66560).fill(97);
const EXAMPLE_WIRE = concat([
  encoder.encode(`10000;chunk-signature=${EXAMPLE_SIGNATURES[0]}\r\n`),
  EXAMPLE_BODY.subarray(0, 65536),
  encoder.encode(`\r\n400;chunk-signature=${EXAMPLE_SIGNATURES[1]}\r\n`),
  EXAMPLE_BODY.subarray(65536),
  encoder.encode(`\r\n0;chunk-signature=${EXAMPLE_SIGNATURES[2]}\r\n\r\n`),
]);

// The same credentials signing other bodies: "hello " and "world" as two chunks, with a trailer
// section of `x-amz-checksum-crc32:DUoRhQ==`; and "hello world" as one chunk, with no trailer.
const TRAILER_SIGNATURES = [
  '36a9919e5e855a54fc5e137c919e524764c71ade46792d228f7ebaade71602ab',
  '927d7061508e51b79d4e40eb55aed9033cd0c471859241fb0355ca3c9ff53a92',
  'ae13d294fc309f1858acb23b717c250dc572e78820dfd4bf5bae1dcb2055298f',
  'bf13d348abc27fe00099cbe7080e9a425fe4d33ec9a9225e6cc86a0a8bc2099a',
];
const PLAIN_SIGNATURES = [
  '658a49ee7b49af44c11311d3461346a5688fb3ce5daa61dd95377587f0149255',
  '1172018d76cd25eb7956fc92d1935d976024a1d96841533ea7b72efe5e8d7463',
];

const WIRE_BODY = encoder.encode('hello world');

// STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER: every chunk, the final one included, carries a
// signature, and the trailer has its own.
function signedTrailerWire(
  signatures = TRAILER_SIGNATURES,
  checksumLine = 'x-amz-checksum-crc32:DUoRhQ==\r\n'
) {
  return encoder.encode(
    `6;chunk-signature=${signatures[0]}\r\nhello \r\n` +
      `5;chunk-signature=${signatures[1]}\r\nworld\r\n` +
      `0;chunk-signature=${signatures[2]}\r\n` +
      checksumLine +
      `x-amz-trailer-signature:${signatures[3]}\r\n` +
      '\r\n'
  );
}

// Wire bytes written out by hand, rather than by encode(), in the forms S3 clients send.
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

    const signed = new AwsChunkedDecoderStream({
      decodedLength: 11,
      allowedTrailers: ['x-amz-checksum-crc32'],
      signing: SIGNING,
    });
    deepStrictEqual(await run(signed, signedTrailerWire(), 7), WIRE_BODY);
    const trailers = await signed.trailers;
    strictEqual(trailers.get('x-amz-checksum-crc32'), 'DUoRhQ==');
    strictEqual([...trailers].length, 1);

    // STREAMING-AWS4-HMAC-SHA256-PAYLOAD, with no trailer.
    const plain = new AwsChunkedDecoderStream({
      decodedLength: 11,
      signing: SIGNING,
    });
    const plainWire = encoder.encode(
      `b;chunk-signature=${PLAIN_SIGNATURES[0]}\r\nhello world\r\n` +
        `0;chunk-signature=${PLAIN_SIGNATURES[1]}\r\n\r\n`
    );
    deepStrictEqual(await run(plain, plainWire, 5), WIRE_BODY);
    strictEqual([...(await plain.trailers)].length, 0);
  },
};

export const signedAwsExample = {
  async test() {
    for (const writeSize of [1000, 4096, 65536, EXAMPLE_WIRE.byteLength]) {
      const stream = new AwsChunkedDecoderStream({
        decodedLength: EXAMPLE_BODY.byteLength,
        signing: SIGNING,
      });
      deepStrictEqual(await run(stream, EXAMPLE_WIRE, writeSize), EXAMPLE_BODY);
      strictEqual([...(await stream.trailers)].length, 0);
    }

    // Piped between native streams.
    const piped = new AwsChunkedDecoderStream({
      decodedLength: EXAMPLE_BODY.byteLength,
      signing: SIGNING,
    });
    const output = new Uint8Array(
      await new Response(
        new Response(EXAMPLE_WIRE).body.pipeThrough(piped)
      ).arrayBuffer()
    );
    deepStrictEqual(output, EXAMPLE_BODY);
  },
};

async function rejectsWith(
  input,
  message,
  options = { decodedLength: BODY.byteLength }
) {
  const stream = new AwsChunkedDecoderStream(options);
  await rejects(run(stream, input), (e) => {
    ok(e instanceof TypeError, `${e}`);
    ok(message.test(e.message), e.message);
    return true;
  });
  await rejects(stream.trailers, TypeError);
}

const MISMATCH = /An aws-chunked chunk or trailer signature does not match/;
const MALFORMED = /Malformed aws-chunked body/;

export const signatureMismatch = {
  async test() {
    const options = {
      decodedLength: EXAMPLE_BODY.byteLength,
      signing: SIGNING,
    };

    // One changed byte in the second chunk.
    const tampered = EXAMPLE_WIRE.slice();
    tampered[tampered.byteLength - 200] = 98;
    await rejectsWith(tampered, MISMATCH, options);

    // A verified chunk is output, and the one after it never is.
    const stream = new AwsChunkedDecoderStream(options);
    const writer = stream.writable.getWriter();
    writer.closed.catch(() => {});
    const reader = stream.readable.getReader();
    const writing = writer.write(tampered).then(() => writer.close());
    let read = 0;
    await rejects(
      (async () => {
        for (;;) {
          const { done, value } = await reader.read();
          if (done) return;
          read += value.byteLength;
        }
      })(),
      MISMATCH
    );
    await rejects(writing, MISMATCH);
    strictEqual(read, 65536);

    // Other credentials.
    await rejectsWith(EXAMPLE_WIRE, MISMATCH, {
      ...options,
      signing: { ...SIGNING, secretAccessKey: 'another key' },
    });
    await rejectsWith(EXAMPLE_WIRE, MISMATCH, {
      ...options,
      signing: { ...SIGNING, region: 'eu-west-1' },
    });
    await rejectsWith(EXAMPLE_WIRE, MISMATCH, {
      ...options,
      signing: { ...SIGNING, seedSignature: '0'.repeat(64) },
    });

    // A trailer signature that does not match, or a checksum changed under it.
    const trailerOptions = { decodedLength: 11, signing: SIGNING };
    await rejectsWith(
      signedTrailerWire([...TRAILER_SIGNATURES.slice(0, 3), '0'.repeat(64)]),
      MISMATCH,
      trailerOptions
    );
    await rejectsWith(
      signedTrailerWire(
        TRAILER_SIGNATURES,
        'x-amz-checksum-crc32:AAAAAA==\r\n'
      ),
      MISMATCH,
      trailerOptions
    );
  },
};

// A signed body must sign every chunk and its trailer; an unsigned one may carry no signatures.
export const signaturePresence = {
  async test() {
    await rejectsWith(
      encoder.encode(
        `6\r\nhello \r\n0;chunk-signature=${TRAILER_SIGNATURES[2]}\r\n\r\n`
      ),
      MALFORMED,
      { decodedLength: 6, signing: SIGNING }
    );
    await rejectsWith(
      encoder.encode(
        `6;chunk-signature=${TRAILER_SIGNATURES[0]}\r\nhello \r\n` +
          `5;chunk-signature=${TRAILER_SIGNATURES[1]}\r\nworld\r\n` +
          `0;chunk-signature=${TRAILER_SIGNATURES[2]}\r\n` +
          'x-amz-checksum-crc32:DUoRhQ==\r\n\r\n'
      ),
      MALFORMED,
      { decodedLength: 11, signing: SIGNING }
    );

    await rejectsWith(EXAMPLE_WIRE, MALFORMED, {
      decodedLength: EXAMPLE_BODY.byteLength,
    });
    await rejectsWith(signedTrailerWire(), MALFORMED, { decodedLength: 11 });
  },
};

export const signingOptions = {
  test() {
    const create = (signing) =>
      new AwsChunkedDecoderStream({ decodedLength: 0, signing });
    create(SIGNING);
    throws(() => create({ ...SIGNING, amzDate: '2013-05-24' }), {
      name: 'TypeError',
      message: /signing.amzDate must be an x-amz-date/,
    });
    throws(() => create({ ...SIGNING, seedSignature: 'abc' }), {
      name: 'TypeError',
      message: /signing.seedSignature must be 64 hex digits/,
    });
  },
};

// The only chunk extension is a chunk signature.
export const extensions = {
  async test() {
    for (const extension of [
      ';a',
      ';a=b',
      ' ;a',
      ';a="quoted ; value"',
      ';chunk-signature=abc',
      ';chunk-signature=' + 'a'.repeat(64) + ';a',
    ]) {
      await rejectsWith(encode(BODY, { extension }), MALFORMED);
    }
  },
};

export const malformed = {
  async test() {
    const good = encode(BODY, { trailer: [CHECKSUM] });
    for (const input of [
      'zz\r\n',
      '\r\n',
      '5\n',
      '5\r\rhello\r\n',
      // A size of more than 32 bits.
      '1'.repeat(17) + '\r\n',
      '5\r\nhelloX\r\n',
      '5\r\nhello\rX',
    ]) {
      await rejectsWith(encoder.encode(input), MALFORMED);
    }
    await rejectsWith(
      good.subarray(0, 20),
      /ended after 12 of its 44 decoded bytes/
    );
    await rejectsWith(
      good.subarray(0, good.byteLength - CHECKSUM.length - 7),
      /ended before its final chunk/
    );
    // Anything after the final chunk is its trailer section.
    await rejectsWith(concat([good, encoder.encode('x')]), MALFORMED);
    await rejectsWith(encode(BODY, { trailer: ['no colon here'] }), MALFORMED);
    await rejectsWith(encode(BODY, { trailer: ['bad name: x'] }), MALFORMED);
    await rejectsWith(
      encode(BODY, { trailer: ['x-a: \x01'] }),
      /Malformed aws-chunked trailer field value/
    );
    await rejectsWith(
      encode(BODY, { extension: ';' + 'e'.repeat(5000) }),
      /chunk size line is longer than 1024 bytes/
    );
  },
};

// The end of the trailer section is not checked: the stream ends it.
export const unterminatedTrailer = {
  async test() {
    const good = encode(BODY, { trailer: [CHECKSUM] });
    for (const missing of [1, 2, 3]) {
      const stream = new AwsChunkedDecoderStream({
        decodedLength: BODY.byteLength,
      });
      deepStrictEqual(
        await run(stream, good.subarray(0, good.byteLength - missing)),
        BODY
      );
      strictEqual(
        (await stream.trailers).get('x-amz-checksum-crc32'),
        'sOO8/Q=='
      );
    }
  },
};

export const lengthsAndLimits = {
  async test() {
    const input = encode(BODY, { trailer: [CHECKSUM] });
    await rejectsWith(input, /decodes to more than its decoded length/, {
      decodedLength: BODY.byteLength - 1,
    });
    await rejectsWith(input, /ended after 44 of its 45 decoded bytes/, {
      decodedLength: BODY.byteLength + 1,
    });
    await rejectsWith(
      encode(BODY, { trailer: ['x-a:' + 'v'.repeat(100)] }),
      /trailer is longer than 64 bytes/,
      { decodedLength: BODY.byteLength, maxTrailerBytes: 64 }
    );
    // Exactly at the limit is fine: the section, with its final CRLF, is 64 bytes.
    const atLimit = new AwsChunkedDecoderStream({
      decodedLength: BODY.byteLength,
      maxTrailerBytes: 64,
    });
    deepStrictEqual(
      await run(
        atLimit,
        encode(BODY, { trailer: ['x-a:' + 'v'.repeat(56)] }),
        3
      ),
      BODY
    );
    // The options are required, and so is the decoded length in them: a missing one is NaN.
    throws(() => new AwsChunkedDecoderStream(), TypeError);
    throws(() => new AwsChunkedDecoderStream({}), {
      name: 'RangeError',
      message: /decodedLength must be a non-negative integer/,
    });
    throws(
      () => new AwsChunkedDecoderStream({ decodedLength: -1 }),
      RangeError
    );
    throws(
      () =>
        new AwsChunkedDecoderStream({ decodedLength: 0, maxTrailerBytes: 0.5 }),
      RangeError
    );
  },
};

// An error that the end of the input reveals reaches a consumer that reads the whole body, even
// when the decoded length says the body is complete without it, and whether the stream had
// failed before the consumer started reading or fails while it waits.
export const lateFailureWholeBody = {
  async test() {
    const truncated = encode(BODY);
    const cases = [
      [
        { decodedLength: BODY.byteLength },
        encode(BODY, { trailer: ['bad name: x'] }),
        MALFORMED,
      ],
      [
        { decodedLength: BODY.byteLength, drainOnError: false },
        encode(BODY, { trailer: ['bad name: x'] }),
        MALFORMED,
      ],
      [
        { decodedLength: BODY.byteLength },
        truncated.subarray(0, truncated.byteLength - 3),
        /ended before its final chunk/,
      ],
      [{ decodedLength: 0 }, encoder.encode('0\r'), /ended before/],
      [{ decodedLength: 0 }, encoder.encode('0\r\nx\r\n'), MALFORMED],
      [
        { decodedLength: 11, signing: SIGNING },
        signedTrailerWire([...TRAILER_SIGNATURES.slice(0, 3), '0'.repeat(64)]),
        MISMATCH,
      ],
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

    const draining = new AwsChunkedDecoderStream({ decodedLength: 0 });
    const writer = draining.writable.getWriter();
    writer.closed.catch(() => {});
    const reading = new Response(draining.readable).arrayBuffer();
    reading.catch(() => {});
    for (let i = 0; i < bad.byteLength; i += 100) {
      await writer.write(bad.slice(i, i + 100));
    }
    const closing = writer.close();
    writer.ready.catch(() => {});
    await rejects(closing, MALFORMED);
    await rejects(reading, MALFORMED);
    await rejects(draining.trailers, MALFORMED);

    const strict = new AwsChunkedDecoderStream({
      decodedLength: 0,
      drainOnError: false,
    });
    const strictWriter = strict.writable.getWriter();
    strictWriter.closed.catch(() => {});
    new Response(strict.readable).arrayBuffer().catch(() => {});
    await rejects(strictWriter.write(bad), MALFORMED);
    await rejects(strict.trailers, MALFORMED);
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
    const stream = new AwsChunkedDecoderStream({ decodedLength: 5 });
    const writer = stream.writable.getWriter();
    await writer.write(encoder.encode('5\r\nhel'));
    await writer.abort(new Error('client went away'));
    await rejects(stream.trailers);
  },
};
