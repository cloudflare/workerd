// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(req, env) {
    const ev = new EventSource('http://localhost:8888');

    // Note that as a non-standard extension, within workers it is possible to
    // create an EventSource from an existing ReadableStream.
    //
    // const ev = EventSource.from(readable);

    ev.onopen = () => {
      console.log('open!');
    };

    ev.onerror = (event) => {
      console.log('error!', event.error);
    };

    ev.onmessage = (event) => {
      console.log('message!', event.data, event.lastEventId);
    };

    // We'll keep the connection open and running for 20 seconds...
    await scheduler.wait(20000);

    return new Response("Hello World\n");
  }
};
