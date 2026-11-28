// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The worker debugger-driver.mjs drives over DevTools. Breakpoints are placed by the marker
// comments (BREAKPOINT, STEP-OVER, STEP-INTO), which the driver finds in this file's source as
// DevTools reports it, so these functions can move without breaking the test.

function stepTarget() {
  const a = 1; // BREAKPOINT
  const b = stepHelper(a); // STEP-OVER
  return b;
}

function stepHelper(x) {
  return x + 41; // STEP-INTO
}

function debuggerStatement() {
  const local = 'local-value';
  debugger;
  return `after-debugger ${local}`;
}

function idleTarget() {
  return 'idle-target';
}

// Runs long enough for the driver to send a command while it runs. A fixed amount of work rather
// than a deadline: time does not advance while a request's JavaScript runs.
function busyLoop(iterations) {
  let sink = 0;
  for (let i = 0; i < iterations; i++) {
    sink = (sink + i) | 0;
  }
  return sink;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    switch (url.pathname) {
      case '/step':
        return new Response(String(stepTarget()));
      case '/debugger-statement':
        return new Response(debuggerStatement());
      case '/idle-target':
        return new Response(idleTarget());
      case '/console':
        console.log('console-message', 42);
        console.warn('console-warning');
        return new Response('ok');
      case '/warn-header':
        // A non-ASCII header value makes workerd log a warning of its own (not through `console`),
        // which it queues for the inspector while no session is attached.
        new Headers({ 'x-non-ascii': 'é' });
        return new Response('ok');
      case '/subrequest': {
        const response = await env.BACKEND.fetch(
          'http://backend.test/data?x=1',
          {
            headers: { 'x-request': 'value' },
          }
        );
        return new Response(await response.text());
      }
      case '/busy':
        console.log('busy-start');
        busyLoop(Number(url.searchParams.get('iterations') ?? 300_000_000));
        return new Response('busy-done');
      case '/top-level-debugger': {
        const module = await import('./top-level-debugger.mjs');
        return new Response(module.value);
      }
    }
    return new Response('Not found', { status: 404 });
  },
};
