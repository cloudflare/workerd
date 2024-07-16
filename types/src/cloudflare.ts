// This file extends `standards.ts` with specific comments overrides for Cloudflare Workers APIs
// that aren't adequately described by a standard .d.ts file

import { CommentsData } from "./transforms";
export default {
  caches: {
    $: `*
* The Cache API allows fine grained control of reading and writing from the Cloudflare global network cache.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/cache/)
`,
  },
  CacheStorage: {
    $: `*
* The Cache API allows fine grained control of reading and writing from the Cloudflare global network cache.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/cache/)
`,
  },
  Cache: {
    $: `*
* The Cache API allows fine grained control of reading and writing from the Cloudflare global network cache.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/cache/)
`,
    delete: ` [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/cache/#delete) `,
    match: ` [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/cache/#match) `,
    put: ` [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/cache/#put) `,
  },
  crypto: {
    $: `*
* The Web Crypto API provides a set of low-level functions for common cryptographic tasks.
* The Workers runtime implements the full surface of this API, but with some differences in
* the [supported algorithms](https://developers.cloudflare.com/workers/runtime-apis/web-crypto/#supported-algorithms)
* compared to those implemented in most browsers.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/web-crypto/)
`,
  },
  Crypto: {
    $: `*
* The Web Crypto API provides a set of low-level functions for common cryptographic tasks.
* The Workers runtime implements the full surface of this API, but with some differences in
* the [supported algorithms](https://developers.cloudflare.com/workers/runtime-apis/web-crypto/#supported-algorithms)
* compared to those implemented in most browsers.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/web-crypto/)
`,
  },
  performance: {
    $: `*
* The Workers runtime supports a subset of the Performance API, used to measure timing and performance,
* as well as timing of subrequests and other operations.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/performance/)
`,
  },
  Performance: {
    $: `*
* The Workers runtime supports a subset of the Performance API, used to measure timing and performance,
* as well as timing of subrequests and other operations.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/performance/)
`,
    timeOrigin: ` [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/performance/#performancetimeorigin) `,
    now: ` [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/performance/#performancenow) `,
  },
  self: {
    $: undefined,
  },
  navigator: {
    $: undefined,
  },
  origin: {
    $: undefined,
  },
  WorkerGlobalScope: {
    $: undefined,
  },
} satisfies CommentsData;
