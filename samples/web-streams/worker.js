// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import {
  createSyncLoremStream,
  createAsyncLoremStream,
  createSyncLoremByteStream,
  createAsyncLoremByteStream,
  createSyncUppercaseTransform,
  createAsyncUppercaseTransform
} from "streams-util";

export default {
  async fetch(request) {
    const url = new URL(request.url);

    // Byte stream variants (no transform, returned directly)
    if (url.pathname === "/bytes/sync") {
      return new Response(createSyncLoremByteStream(20), {
        headers: { "Content-Type": "text/plain; charset=utf-8" }
      });
    }
    if (url.pathname === "/bytes/async") {
      return new Response(createAsyncLoremByteStream(20), {
        headers: { "Content-Type": "text/plain; charset=utf-8" }
      });
    }

    // Default stream variants with uppercase transform
    let loremStream;
    let transform;
    if (url.pathname === "/sync") {
      loremStream = createSyncLoremStream(20);
      transform = createSyncUppercaseTransform();
    } else if (url.pathname === "/async") {
      loremStream = createAsyncLoremStream(20);
      transform = createAsyncUppercaseTransform();
    } else {
      return new Response("Use /sync, /async, /bytes/sync, or /bytes/async\n", { status: 404 });
    }

    const transformedStream = loremStream.pipeThrough(transform);

    return new Response(transformedStream, {
      headers: { "Content-Type": "text/plain; charset=utf-8" }
    });
  }
};
