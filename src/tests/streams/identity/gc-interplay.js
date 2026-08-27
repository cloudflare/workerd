// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Interplay with garbage collection (the configs set --expose-gc). A
// writer acquired from a stream whose wrapper object has become
// unreachable must keep the underlying stream alive and operable.

export const abortWriterAfterGc = {
  async test() {
    function getWriter() {
      const { writable } = new IdentityTransformStream();
      return writable.getWriter();
    }
    const writer = getWriter();
    gc();
    await writer.abort();
  },
};
