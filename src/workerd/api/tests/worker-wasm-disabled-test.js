// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
export default {
  async test() {
    const bytes = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);
    try {
      await WebAssembly.compile(bytes);
    } catch (error) {
      if (error.message.includes('Wasm code generation disallowed by embedder'))
        return;
      throw error;
    }
    throw new Error(
      'Request-time WebAssembly compilation was enabled without its compatibility flag'
    );
  },
};
