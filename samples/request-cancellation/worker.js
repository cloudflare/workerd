// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(request, env, ctx) {
    // This sets up an event listener that will be called if the client disconnects from your
    // worker.
    request.signal.addEventListener("abort", () => {
      console.log("The request was aborted!");
    });

    const { readable, writable } = new IdentityTransformStream();
    sendPing(writable);
    return new Response(readable, {
      headers: { "Content-Type": "text/plain" },
    });
  },
};

async function sendPing(writable) {
  const writer = writable.getWriter();
  const enc = new TextEncoder();

  for (;;) {
    // Send 'ping' every second to keep the connection alive
    await writer.write(enc.encode("ping\r\n"));
    await scheduler.wait(1000);
  }
}
