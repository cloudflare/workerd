// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(request) {
    request.signal.addEventListener('abort', () => {
      console.log('Request was aborted');
    });

    console.log('Starting long-running task');
    // Simulate a long-running task so we can test aborting
    await new Promise((resolve) => setTimeout(resolve, 3000));

    return new Response("Request was not aborted");
  }
};
