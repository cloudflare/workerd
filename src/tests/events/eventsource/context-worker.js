// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

const originalThen = Promise.prototype.then;
let pending;

async function capture(start) {
  let result;
  Promise.prototype.then = function (resolve, reject) {
    return originalThen.call(this, (value) => {
      result = { resolve, reject, value };
    });
  };
  try {
    start();
    for (let i = 0; result === undefined && i < 100; i++) {
      await scheduler.wait(10);
    }
    if (result === undefined) {
      throw new Error('No fetch continuation was captured');
    }
    return result;
  } finally {
    Promise.prototype.then = originalThen;
  }
}

async function create(env) {
  const state = { events: [], source: undefined, completion: undefined };
  state.completion = await capture(() => {
    state.source = new EventSource('https://fixture/empty', {
      fetcher: env.backend,
    });
    state.source.onopen = () => state.events.push('open');
    state.source.onerror = () => state.events.push('error');
  });
  return state;
}

async function resume(state, kind, env) {
  const response =
    kind === '200'
      ? (await capture(() => env.backend.fetch('https://fixture/body'))).value
      : state.completion.value;
  state.completion.resolve(response);
  for (let i = 0; i < 4; i++) await 0;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const kind = url.searchParams.get('kind');
    if (url.pathname === '/body' || url.pathname === '/empty') {
      const body = url.pathname === '/body';
      return new Response(body ? 'data: message\n\n' : null, {
        status: body ? 200 : 204,
        headers: { 'content-type': 'text/event-stream' },
      });
    }
    if (url.pathname === '/create') {
      pending = await create(env);
      return new Response('created');
    }
    if (url.pathname === '/resume') {
      // Yield once in request B so request A's teardown has completed.
      await scheduler.wait(20);
      await resume(pending, kind, env);
      let closeError;
      try {
        pending.source.close();
      } catch (error) {
        closeError = error.message;
      }
      const result = {
        events: [...pending.events],
        readyState: pending.source.readyState,
        closeError,
      };
      pending = undefined;
      return Response.json(result);
    }
    if (url.pathname === '/retained') {
      const state = await create(env);
      await resume(state, kind, env);
      state.source.close();
      state.source.close();
      return Response.json({
        events: [...state.events],
        readyState: state.source.readyState,
      });
    }
    throw new Error('Unknown path');
  },
};
