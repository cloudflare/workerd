// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { Buffer } from 'node:buffer';
import zlib from 'node:zlib';

function initializeNative(decompress, parameter, value) {
  const stream = decompress
    ? zlib.createZstdDecompress()
    : zlib.createZstdCompress();
  const Handle = stream._handle.constructor;
  stream.close();
  const handle = new Handle(
    decompress ? zlib.constants.ZSTD_DECOMPRESS : zlib.constants.ZSTD_COMPRESS
  );
  try {
    let errorCode;
    handle.setErrorHandler((_message, code) => {
      errorCode = code;
    });
    const params = new Int32Array(parameter + 1).fill(-1);
    params[parameter] = value;
    const initialized = handle.initialize(params, new Uint32Array(2), () => {});
    return { initialized, errorCode };
  } finally {
    handle.close();
  }
}

export const rejectsUnsupportedNativeParameters = {
  test() {
    // Initialize only: no writes are made with unsupported buffer or frame modes.
    const unsupported = [
      0, 10, 99, 108, 129, 131, 199, 203, 299, 403, 500, 999,
    ];
    for (let parameter = 1000; parameter <= 1025; parameter++) {
      unsupported.push(parameter);
    }
    for (const decompress of [false, true]) {
      for (const parameter of unsupported) {
        assert.deepStrictEqual(initializeNative(decompress, parameter, 1), {
          initialized: false,
          errorCode: 'ERR_ZSTD_PARAM_SET_FAILED',
        });
      }
    }
  },
};

export const rejectsUnsupportedOneShotParameters = {
  async test() {
    const input = Buffer.from('Zstd parameter validation');
    const compressed = zlib.zstdCompressSync(input);
    for (const decompress of [false, true]) {
      const sync = decompress ? zlib.zstdDecompressSync : zlib.zstdCompressSync;
      const callback = decompress ? zlib.zstdDecompress : zlib.zstdCompress;
      const data = decompress ? compressed : input;
      for (const parameter of [
        -1, 10, 500, 1000, 1001, 1006, 1007, 2147483647,
      ]) {
        const options = { params: { [parameter]: 1 } };
        const message =
          /Unsupported Zstd (compression|decompression) parameter/;
        assert.throws(() => sync(data, options), message);
        await new Promise((resolve, reject) => {
          callback(data, options, (error) => {
            try {
              assert.match(error?.message, message);
              resolve();
            } catch (error) {
              reject(error);
            }
          });
        });
      }
    }
  },
};

export const preservesSupportedParameters = {
  test() {
    const input = Buffer.from('Zstd supported parameters');
    const compressed = zlib.zstdCompressSync(input);
    for (const decompress of [false, true]) {
      const prefix = decompress ? 'ZSTD_d_' : 'ZSTD_c_';
      const parameters = Object.entries(zlib.constants)
        .filter(([name]) => name.startsWith(prefix))
        .map(([, value]) => value);
      // Stable since Zstandard 1.5.6, also accepted as a numeric API parameter.
      if (!decompress) parameters.push(130); // ZSTD_c_targetCBlockSize
      for (const parameter of new Set(parameters)) {
        assert.deepStrictEqual(initializeNative(decompress, parameter, 0), {
          initialized: true,
          errorCode: undefined,
        });
        const options = { params: { [parameter]: 0 } };
        const result = decompress
          ? zlib.zstdDecompressSync(compressed, options)
          : zlib.zstdDecompressSync(zlib.zstdCompressSync(input, options));
        assert.deepStrictEqual(result, input);
      }
    }
    // Allowed keys still receive Zstandard's value validation.
    assert.deepStrictEqual(
      initializeNative(false, zlib.constants.ZSTD_c_strategy, 2147483647),
      { initialized: false, errorCode: 'ERR_ZSTD_PARAM_SET_FAILED' }
    );
  },
};
