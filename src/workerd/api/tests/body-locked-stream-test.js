// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { throws } from 'assert';

// WPT covers new Response(lockedStream). Its Request counterpart passes regardless, because the
// relative URL it uses is rejected first, so the Request path is covered here.
export const lockedStreamRequestBody = {
  test() {
    const stream = new ReadableStream();
    stream.getReader();
    throws(
      () =>
        new Request('https://example.com', {
          method: 'POST',
          body: stream,
        }),
      { name: 'TypeError', message: /locked to a reader/ }
    );
  },
};
