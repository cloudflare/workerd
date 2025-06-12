// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';

export default class TailWorker extends WorkerEntrypoint {
  fetch() {
    console.log('Fetch event received in Tail Worker');
    return new Response('Hello from Tail Worker!');
  }
  
  tail(events) {
    console.log('Tail event received in Tail Worker:', events);
  } 
}

// Same issue with non WorkerEntrypoint export
// export default {
//     fetch() {
//         return new Response('Hello from Tail Worker!');
//     },
//     tail(events) {
//         console.log('Tail event received:', events);
//     } 
// }
