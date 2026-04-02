// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint, RpcTarget } from 'cloudflare:workers';

const SEARCH_DATA = [
  {
    file_id: 'file-12345',
    filename: 'woodchuck_policy.txt',
    score: 0.85,
    attributes: {
      region: 'North America',
      author: 'Wildlife Department',
    },
    content: [
      {
        type: 'text',
        text: 'According to the latest regulations, each passenger is allowed to carry up to two woodchucks.',
      },
      {
        type: 'text',
        text: 'Ensure that the woodchucks are properly contained during transport.',
      },
    ],
  },
  {
    file_id: 'file-67890',
    filename: 'transport_guidelines.txt',
    score: 0.75,
    attributes: {
      region: 'North America',
      author: 'Transport Authority',
    },
    content: [
      {
        type: 'text',
        text: 'Passengers must adhere to the guidelines set forth by the Transport Authority regarding the transport of woodchucks.',
      },
    ],
  },
];

const SEARCH_RESPONSE = {
  object: 'vector_store.search_results.page',
  search_query: 'How many woodchucks are allowed per passenger?',
  data: SEARCH_DATA,
  has_more: false,
  next_page: null,
};

/** RPC target returned by autorag(autoragId) */
class AutoRAGInstance extends RpcTarget {
  constructor(_autoragId) {
    super();
  }

  async search(params) {
    return {
      ...SEARCH_RESPONSE,
    };
  }

  async aiSearch(params) {
    return {
      ...SEARCH_RESPONSE,
      response: 'this is an example result',
    };
  }
}

/** WorkerEntrypoint that supports both fetch (legacy) and RPC (beta) paths */
export class ServiceEntrypoint extends WorkerEntrypoint {
  autorag(autoragId) {
    return new AutoRAGInstance(autoragId);
  }

  async fetch(request) {
    if (request.method === 'POST') {
      if (request.url.includes('/ai-search')) {
        return Response.json({
          result: {
            ...SEARCH_RESPONSE,
            response: 'this is an example result',
          },
          success: true,
        });
      }

      if (request.url.includes('/search')) {
        return Response.json({
          result: SEARCH_RESPONSE,
          success: true,
        });
      }
    }

    return Response.json({ success: false }, { status: 500 });
  }
}

export default {
  async fetch(request, env, ctx) {
    const entrypoint = new ServiceEntrypoint(ctx, env);
    return entrypoint.fetch(request);
  },
};
