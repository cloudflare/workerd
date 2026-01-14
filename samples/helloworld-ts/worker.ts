// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export default {
  async fetch(_request, _env, _ctx): Promise<Response> {
    return new Response('Hello World from Typescript!')
  },
} satisfies ExportedHandler<Env>
