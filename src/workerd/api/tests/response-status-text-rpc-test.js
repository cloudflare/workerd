// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { strictEqual } from 'node:assert';
import { WorkerEntrypoint } from 'cloudflare:workers';

// pedantic_wpt changes which status messages a Worker may construct a Response with. A Response
// that crosses to a Worker with the other setting must arrive intact and remain usable there,
// including as the init of a new Response, which is how Workers proxy responses.

// Each status message is accepted only by the sender's rules.
const PEDANTIC_ONLY = ['a\tb'];
const NON_PEDANTIC_ONLY = ['Ā', 'a\x7fb'];

function statusTextsFromPeer(env) {
  // Latin-1 characters (obs-text) are accepted by both.
  return [...(env.peerIsPedantic ? PEDANTIC_ONLY : NON_PEDANTIC_ONLY), 'é'];
}

function makeResponse(statusText) {
  return new Response(null, { status: 200, statusText });
}

function checkReceived(response, statusText) {
  strictEqual(response.statusText, statusText);
  strictEqual(new Response(null, response).statusText, statusText);
  strictEqual(Response.json(null, response).statusText, statusText);
}

export class Peer extends WorkerEntrypoint {
  makeResponse(statusText) {
    return makeResponse(statusText);
  }

  // Passes a Response to the caller as an RPC argument.
  sendResponseToPeer(statusText) {
    return this.env.peer.checkReceived(makeResponse(statusText), statusText);
  }

  checkReceived(response, statusText) {
    checkReceived(response, statusText);
  }

  fetch(request) {
    return makeResponse(new URL(request.url).searchParams.get('statusText'));
  }
}

export const returnedOverRpc = {
  async test(ctrl, env) {
    for (const statusText of statusTextsFromPeer(env)) {
      checkReceived(await env.peer.makeResponse(statusText), statusText);
    }
  },
};

export const passedOverRpc = {
  async test(ctrl, env) {
    for (const statusText of statusTextsFromPeer(env)) {
      await env.peer.sendResponseToPeer(statusText);
    }
  },
};

export const fetchedOverServiceBinding = {
  async test(ctrl, env) {
    for (const statusText of statusTextsFromPeer(env)) {
      const url = `http://peer/?${new URLSearchParams({ statusText })}`;
      checkReceived(await env.peer.fetch(url), statusText);
    }
  },
};
