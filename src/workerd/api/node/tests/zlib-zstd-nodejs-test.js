// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';
import { Buffer } from 'node:buffer';
import zlib from 'node:zlib';

// Basic sync compress/decompress test
export const zstdBasicSyncTest = {
  test() {
    const input = Buffer.from('Hello, Zstd compression!');
    const compressed = zlib.zstdCompressSync(input);
    assert(Buffer.isBuffer(compressed), 'Compressed output should be a buffer');
    assert(compressed.length > 0, 'Compressed output should not be empty');

    const decompressed = zlib.zstdDecompressSync(compressed);
    assert(
      Buffer.isBuffer(decompressed),
      'Decompressed output should be a buffer'
    );
    assert.strictEqual(
      decompressed.toString(),
      input.toString(),
      'Round-trip should match'
    );
  },
};

// Basic async compress/decompress test
export const zstdBasicAsyncTest = {
  async test() {
    const input = Buffer.from('Hello, async Zstd compression!');

    const {
      promise: compressPromise,
      resolve: compressResolve,
      reject: compressReject,
    } = Promise.withResolvers();
    zlib.zstdCompress(input, (err, res) => {
      if (err) compressReject(err);
      else compressResolve(res);
    });
    const compressed = await compressPromise;

    assert(Buffer.isBuffer(compressed), 'Compressed output should be a buffer');
    assert(compressed.length > 0, 'Compressed output should not be empty');

    const {
      promise: decompressPromise,
      resolve: decompressResolve,
      reject: decompressReject,
    } = Promise.withResolvers();
    zlib.zstdDecompress(compressed, (err, res) => {
      if (err) decompressReject(err);
      else decompressResolve(res);
    });
    const decompressed = await decompressPromise;

    assert(
      Buffer.isBuffer(decompressed),
      'Decompressed output should be a buffer'
    );
    assert.strictEqual(
      decompressed.toString(),
      input.toString(),
      'Round-trip should match'
    );
  },
};

// Test with string input
export const zstdStringInputTest = {
  test() {
    const input = 'This is a string input for Zstd compression';
    const compressed = zlib.zstdCompressSync(input);
    const decompressed = zlib.zstdDecompressSync(compressed);
    assert.strictEqual(
      decompressed.toString(),
      input,
      'String input round-trip should match'
    );
  },
};

// Test with larger data
export const zstdLargeDataTest = {
  test() {
    // Create a 100KB buffer with repetitive content (compresses well)
    const input = Buffer.alloc(100 * 1024);
    for (let i = 0; i < input.length; i++) {
      input[i] = i % 256;
    }

    const compressed = zlib.zstdCompressSync(input);
    assert(
      compressed.length < input.length,
      'Compressed should be smaller than input'
    );

    const decompressed = zlib.zstdDecompressSync(compressed);
    assert(input.equals(decompressed), 'Large data round-trip should match');
  },
};

// Test with different compression levels
export const zstdCompressionLevelsTest = {
  test() {
    const input = Buffer.from(
      'Test data for compression level testing'.repeat(100)
    );

    // Test compression level 1 (fastest)
    const compressedFast = zlib.zstdCompressSync(input, {
      params: { [zlib.constants.ZSTD_c_compressionLevel]: 1 },
    });

    // Test compression level 19 (best compression)
    const compressedBest = zlib.zstdCompressSync(input, {
      params: { [zlib.constants.ZSTD_c_compressionLevel]: 19 },
    });

    // Both should decompress correctly
    const decompressedFast = zlib.zstdDecompressSync(compressedFast);
    const decompressedBest = zlib.zstdDecompressSync(compressedBest);

    assert(
      input.equals(decompressedFast),
      'Fast compression should decompress correctly'
    );
    assert(
      input.equals(decompressedBest),
      'Best compression should decompress correctly'
    );

    // Higher compression level should typically produce smaller output
    assert(
      compressedBest.length <= compressedFast.length,
      'Higher compression level should produce smaller or equal output'
    );
  },
};

// Test with default compression level
export const zstdDefaultCompressionTest = {
  test() {
    const input = Buffer.from('Testing default compression level');
    const compressed = zlib.zstdCompressSync(input);
    const decompressed = zlib.zstdDecompressSync(compressed);
    assert.strictEqual(
      decompressed.toString(),
      input.toString(),
      'Default compression should work'
    );
  },
};

// Test stream API
export const zstdStreamTest = {
  async test() {
    const input = Buffer.from('Stream compression test data'.repeat(50));

    // Create compress stream
    const compress = zlib.createZstdCompress();
    const decompress = zlib.createZstdDecompress();

    const chunks = [];

    // Pipe through compress
    compress.on('data', (chunk) => chunks.push(chunk));

    await new Promise((resolve, reject) => {
      compress.on('end', resolve);
      compress.on('error', reject);
      compress.end(input);
    });

    const compressed = Buffer.concat(chunks);
    assert(compressed.length > 0, 'Stream should produce output');

    // Decompress
    const decompressedChunks = [];
    decompress.on('data', (chunk) => decompressedChunks.push(chunk));

    await new Promise((resolve, reject) => {
      decompress.on('end', resolve);
      decompress.on('error', reject);
      decompress.end(compressed);
    });

    const decompressed = Buffer.concat(decompressedChunks);
    assert(input.equals(decompressed), 'Stream round-trip should match');
  },
};

// Test empty input
export const zstdEmptyInputTest = {
  test() {
    const input = Buffer.alloc(0);
    const compressed = zlib.zstdCompressSync(input);
    const decompressed = zlib.zstdDecompressSync(compressed);
    assert.strictEqual(
      decompressed.length,
      0,
      'Empty input should produce empty output'
    );
  },
};

// Test invalid compressed data
export const zstdInvalidDataTest = {
  test() {
    const invalidData = Buffer.from('This is not valid zstd compressed data');
    assert.throws(
      () => zlib.zstdDecompressSync(invalidData),
      /Zstd decompression failed/,
      'Should throw on invalid compressed data'
    );
  },
};

// Test chunkSize option
export const zstdChunkSizeTest = {
  test() {
    const input = Buffer.from('Testing chunk size option'.repeat(100));
    const compressed = zlib.zstdCompressSync(input, { chunkSize: 1024 });
    const decompressed = zlib.zstdDecompressSync(compressed, {
      chunkSize: 1024,
    });
    assert(
      input.equals(decompressed),
      'Custom chunkSize should work correctly'
    );
  },
};

// Test maxOutputLength option
export const zstdMaxOutputLengthTest = {
  test() {
    const input = Buffer.from('A'.repeat(1000));
    const compressed = zlib.zstdCompressSync(input);

    // Try to decompress with a maxOutputLength that's too small
    assert.throws(
      () => zlib.zstdDecompressSync(compressed, { maxOutputLength: 10 }),
      /Memory limit exceeded/,
      'Should throw when maxOutputLength is exceeded'
    );
  },
};

// Test callback error handling
export const zstdCallbackErrorTest = {
  async test() {
    const invalidData = Buffer.from('invalid zstd data');

    const { promise, resolve, reject } = Promise.withResolvers();
    zlib.zstdDecompress(invalidData, (err, res) => {
      if (err) reject(err);
      else resolve(res);
    });

    try {
      await promise;
      assert.fail('Should have thrown');
    } catch (err) {
      assert(err instanceof Error, 'Should receive an error');
    }
  },
};

// Test with params for strategy
export const zstdStrategyTest = {
  test() {
    const input = Buffer.from('Testing compression strategy'.repeat(50));

    // Test with ZSTD_fast strategy
    const compressedFast = zlib.zstdCompressSync(input, {
      params: {
        [zlib.constants.ZSTD_c_strategy]: zlib.constants.ZSTD_fast,
      },
    });

    const decompressed = zlib.zstdDecompressSync(compressedFast);
    assert(input.equals(decompressed), 'Strategy option should work correctly');
  },
};

// Test with info option
export const zstdInfoOptionTest = {
  test() {
    const input = Buffer.from('Testing info option');
    const result = zlib.zstdCompressSync(input, { info: true });

    // When info is true, result should be an object with buffer and engine properties
    assert(
      typeof result === 'object',
      'Result should be an object when info is true'
    );
    assert(result.buffer, 'Result should have a buffer property');
    assert(result.engine, 'Result should have an engine property');
  },
};

// Test classes are exported
export const zstdClassesExportedTest = {
  test() {
    assert.strictEqual(
      typeof zlib.ZstdCompress,
      'function',
      'ZstdCompress should be exported'
    );
    assert.strictEqual(
      typeof zlib.ZstdDecompress,
      'function',
      'ZstdDecompress should be exported'
    );
    assert.strictEqual(
      typeof zlib.createZstdCompress,
      'function',
      'createZstdCompress should be exported'
    );
    assert.strictEqual(
      typeof zlib.createZstdDecompress,
      'function',
      'createZstdDecompress should be exported'
    );
  },
};

// Test sync functions are exported
export const zstdSyncFunctionsExportedTest = {
  test() {
    assert.strictEqual(
      typeof zlib.zstdCompressSync,
      'function',
      'zstdCompressSync should be exported'
    );
    assert.strictEqual(
      typeof zlib.zstdDecompressSync,
      'function',
      'zstdDecompressSync should be exported'
    );
    assert.strictEqual(
      typeof zlib.zstdCompress,
      'function',
      'zstdCompress should be exported'
    );
    assert.strictEqual(
      typeof zlib.zstdDecompress,
      'function',
      'zstdDecompress should be exported'
    );
  },
};

// Test sync decompression of truncated compressed data (EOF mid-frame)
export const zstdTruncatedSyncTest = {
  test() {
    const input = Buffer.from('Hello, Zstd truncation test!'.repeat(100));
    const compressed = zlib.zstdCompressSync(input);

    // Cut the compressed data in half
    const truncated = compressed.subarray(0, Math.floor(compressed.length / 2));
    assert.throws(
      () => zlib.zstdDecompressSync(truncated),
      (err) => err instanceof Error,
      'Should throw on truncated compressed data'
    );
  },
};

// Test async decompression of truncated compressed data
export const zstdTruncatedAsyncTest = {
  async test() {
    const input = Buffer.from('Hello, Zstd truncation test!'.repeat(100));
    const compressed = zlib.zstdCompressSync(input);
    const truncated = compressed.subarray(0, Math.floor(compressed.length / 2));

    const { promise, resolve, reject } = Promise.withResolvers();
    zlib.zstdDecompress(truncated, (err, res) => {
      if (err) reject(err);
      else resolve(res);
    });

    try {
      await promise;
      assert.fail('Should have thrown on truncated data');
    } catch (err) {
      assert(
        err instanceof Error,
        'Should receive an error for truncated data'
      );
    }
  },
};

// Test stream decompression of truncated compressed data
export const zstdTruncatedStreamTest = {
  async test() {
    const input = Buffer.from('Stream truncation test data'.repeat(100));
    const compressed = zlib.zstdCompressSync(input);
    const truncated = compressed.subarray(0, Math.floor(compressed.length / 2));

    const decompress = zlib.createZstdDecompress();

    let errored = false;
    decompress.on('error', () => {
      errored = true;
    });

    decompress.end(truncated);
    try {
      for await (const _chunk of decompress) {
        // consume chunks
      }
    } catch {
      errored = true;
    }

    assert(errored, 'Stream should error on truncated compressed data');
  },
};

// Test stream decompression with large output and small chunkSize to exercise
// the processCallback recursion path; verify we don't get a stack overflow.
export const zstdStreamLargeDecompressTest = {
  async test() {
    // 1MB of compressible data — compresses small, decompresses large.
    // With chunkSize=64, this requires ~16,000 output chunks, which triggers
    // deep recursion in processCallback if the callback is synchronous.
    const input = Buffer.alloc(1024 * 1024, 'A');
    const compressed = zlib.zstdCompressSync(input);

    const decompress = zlib.createZstdDecompress({ chunkSize: 64 });
    let totalBytes = 0;

    decompress.end(compressed);
    for await (const chunk of decompress) {
      totalBytes += chunk.length;
    }

    assert.strictEqual(
      totalBytes,
      input.length,
      `Decompressed size should be ${input.length}, got ${totalBytes}`
    );
  },
};

function xorshiftBytes(length) {
  const bytes = Buffer.allocUnsafe(length);
  let x = length;
  for (let i = 0; i < length; i++) {
    x ^= x << 13;
    x ^= x >>> 17;
    x ^= x << 5;
    bytes[i] = x & 0x7f;
  }
  return bytes;
}

// Writes each chunk to the stream, ends it and returns everything it outputs.
async function collectStream(stream, chunks) {
  const output = [];
  stream.on('data', (chunk) => output.push(chunk));
  const { promise, resolve, reject } = Promise.withResolvers();
  stream.on('end', resolve);
  stream.on('error', reject);
  for (const chunk of chunks) stream.write(chunk);
  stream.end();
  await promise;
  return Buffer.concat(output);
}

function zstdCompressStreaming(input, options, writeSize) {
  const chunks = [];
  for (let offset = 0; offset < input.length; offset += writeSize) {
    chunks.push(input.subarray(offset, offset + writeSize));
  }
  return collectStream(zlib.createZstdCompress(options), chunks);
}

export const zstdStreamFinalFlushOverflowsChunkTest = {
  async test() {
    const input = xorshiftBytes(200_000);
    const compressed = await zstdCompressStreaming(
      input,
      { chunkSize: 1024 },
      64 * 1024
    );
    assert(compressed.length > 1024);
    assert(input.equals(zlib.zstdDecompressSync(compressed)));
  },
};

export const zstdStreamFinalFlushCrossesMiBChunkBoundaryTest = {
  async test() {
    const input = xorshiftBytes(1_250_000);
    const compressed = await zstdCompressStreaming(
      input,
      {
        chunkSize: 1 << 20,
        params: { [zlib.constants.ZSTD_c_compressionLevel]: 1 },
      },
      64 * 1024
    );
    assert(compressed.length > 1 << 20);
    assert(input.equals(zlib.zstdDecompressSync(compressed)));
  },
};

export const zstdCompressSyncOutputLargerThanFirstBufferTest = {
  test() {
    for (const length of [100_000, 2_000_000]) {
      const input = xorshiftBytes(length);
      const compressed = zlib.zstdCompressSync(input);
      assert(compressed.length > 40 * 1024);
      assert(input.equals(zlib.zstdDecompressSync(compressed)));
    }
  },
};

export const zstdCompressCallbackOutputLargerThanFirstBufferTest = {
  async test() {
    for (const length of [100_000, 2_000_000]) {
      const input = xorshiftBytes(length);
      const { promise, resolve, reject } = Promise.withResolvers();
      zlib.zstdCompress(input, (err, res) => {
        if (err) reject(err);
        else resolve(res);
      });
      const compressed = await promise;
      assert(compressed.length > 40 * 1024);
      assert(input.equals(zlib.zstdDecompressSync(compressed)));
    }
  },
};

export const zstdCompressSyncMaxOutputLengthTest = {
  test() {
    assert.throws(
      () =>
        zlib.zstdCompressSync(xorshiftBytes(200_000), {
          maxOutputLength: 64 * 1024,
        }),
      { name: 'RangeError', message: /Memory limit exceeded/ }
    );
  },
};

export const zstdCompressSyncMaxOutputLengthAtBufferBoundaryTest = {
  test() {
    const syncFirstBufferSize = 40 * 1024;
    assert.throws(
      () =>
        zlib.zstdCompressSync(xorshiftBytes(200_000), {
          maxOutputLength: 2 * syncFirstBufferSize,
        }),
      { name: 'RangeError', message: /Memory limit exceeded/ }
    );
  },
};

// The sync output buffer starts at 40 KiB and doubles, so 1x, 2x and 4x fill it exactly.
export const zstdDecompressSyncOutputFillsBufferTest = {
  async test() {
    const syncFirstBufferSize = 40 * 1024;
    for (const multiple of [1, 2, 3, 4]) {
      for (const delta of [-1, 0, 1]) {
        const size = multiple * syncFirstBufferSize + delta;
        const input = xorshiftBytes(size);
        const compressed = zlib.zstdCompressSync(input);
        assert(
          input.equals(zlib.zstdDecompressSync(compressed)),
          `size ${size}`
        );

        const { promise, resolve, reject } = Promise.withResolvers();
        zlib.zstdDecompress(compressed, (err, res) => {
          if (err) reject(err);
          else resolve(res);
        });
        assert(input.equals(await promise), `size ${size}`);
      }
    }
  },
};

function zstdDecompressCallback(input, options = {}) {
  const { promise, resolve, reject } = Promise.withResolvers();
  zlib.zstdDecompress(input, options, (err, res) => {
    if (err) reject(err);
    else resolve(res);
  });
  return promise;
}

function zstdDecompressStreaming(chunks) {
  return collectStream(zlib.createZstdDecompress(), chunks);
}

// Decodes the input with zstdDecompressSync, zstdDecompress and a single stream write.
async function zstdDecompressAllWays(input) {
  return [
    zlib.zstdDecompressSync(input),
    await zstdDecompressCallback(input),
    await zstdDecompressStreaming([input]),
  ];
}

function skippableFrame(contentLength) {
  const frame = Buffer.alloc(8 + contentLength, 0xab);
  frame.writeUInt32LE(0x184d2a53, 0);
  frame.writeUInt32LE(contentLength, 4);
  return frame;
}

// Node decodes every frame of concatenated input, skipping skippable frames.
export const zstdDecompressConcatenatedFramesTest = {
  async test() {
    const parts = [
      xorshiftBytes(1000),
      xorshiftBytes(50_000),
      xorshiftBytes(7),
    ];
    const frames = parts.map((part) => zlib.zstdCompressSync(part));
    const cases = [
      [frames.slice(0, 2), parts.slice(0, 2)],
      [frames, parts],
      [[skippableFrame(16), frames[0]], [parts[0]]],
      [[frames[0], skippableFrame(0), frames[1]], parts.slice(0, 2)],
      [[frames[0], skippableFrame(16)], [parts[0]]],
    ];
    for (const [inputs, outputs] of cases) {
      const input = Buffer.concat(inputs);
      const expected = Buffer.concat(outputs);
      for (const output of await zstdDecompressAllWays(input)) {
        assert(expected.equals(output));
      }
    }
  },
};

export const zstdDecompressStreamFrameSplitAcrossWritesTest = {
  async test() {
    const parts = [xorshiftBytes(1000), xorshiftBytes(2000)];
    const first = zlib.zstdCompressSync(parts[0]);
    const input = Buffer.concat([first, zlib.zstdCompressSync(parts[1])]);
    const expected = Buffer.concat(parts);
    for (let offset = 1; offset <= 8; offset++) {
      const cut = first.length + offset;
      const output = await zstdDecompressStreaming([
        input.subarray(0, cut),
        input.subarray(cut),
      ]);
      assert(expected.equals(output), `offset ${offset}`);
    }
  },
};

// Like Node without rejectGarbageAfterEnd, input after the last frame that doesn't start another
// frame is ignored.
export const zstdDecompressTrailingJunkTest = {
  async test() {
    const part = xorshiftBytes(1000);
    const frame = zlib.zstdCompressSync(part);
    for (const junk of ['j', 'junk', 'not a zstd frame']) {
      const input = Buffer.concat([frame, Buffer.from(junk)]);
      for (const output of await zstdDecompressAllWays(input)) {
        assert(part.equals(output), junk);
      }
    }
  },
};

export const zstdDecompressTruncatedSecondFrameTest = {
  async test() {
    const frames = [xorshiftBytes(1000), xorshiftBytes(2000)].map((part) =>
      zlib.zstdCompressSync(part)
    );
    const second = frames[1].subarray(0, frames[1].length - 4);
    const input = Buffer.concat([frames[0], second]);
    assert.throws(
      () => zlib.zstdDecompressSync(input),
      /unexpected end of file/
    );
    await assert.rejects(
      zstdDecompressCallback(input),
      /unexpected end of file/
    );
    await assert.rejects(
      zstdDecompressStreaming([input]),
      /unexpected end of file/
    );
  },
};

export const zstdDecompressMaxOutputLengthAcrossFramesTest = {
  test() {
    const parts = [xorshiftBytes(1000), xorshiftBytes(2000)];
    const input = Buffer.concat(
      parts.map((part) => zlib.zstdCompressSync(part))
    );
    assert.throws(
      () => zlib.zstdDecompressSync(input, { maxOutputLength: 1000 }),
      { name: 'RangeError' }
    );
    assert(
      Buffer.concat(parts).equals(
        zlib.zstdDecompressSync(input, { maxOutputLength: 3000 })
      )
    );
  },
};

// Ignored input after a frame doesn't count against maxOutputLength, even when the frame fills it.
export const zstdDecompressMaxOutputLengthFilledBeforeIgnoredInputTest = {
  test() {
    const part = xorshiftBytes(1000);
    const frame = zlib.zstdCompressSync(part);
    for (const after of [
      Buffer.from('junk'),
      Buffer.from([0x28, 0xb5]),
      skippableFrame(16),
    ]) {
      const input = Buffer.concat([frame, after]);
      assert(
        part.equals(zlib.zstdDecompressSync(input, { maxOutputLength: 1000 })),
        after.toString('hex')
      );
    }
  },
};

// Node throws "unexpected end of file" here. workerd has always returned an empty buffer, and
// decoding several frames doesn't change that.
export const zstdDecompressEmptyInputTest = {
  test() {
    assert.strictEqual(zlib.zstdDecompressSync(Buffer.alloc(0)).length, 0);
  },
};
