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
