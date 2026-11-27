// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Uses node:inspector's in-process Session in an isolate that a DevTools session may be attached
// to. Each route reports what it saw as JSON for debugger-driver.mjs to check.
import { Session } from 'node:inspector';

function sessionDispatch() {
  const session = new Session();
  session.connect();
  let contextId;
  session.once('Runtime.executionContextCreated', (message) => {
    contextId = message.params.context.id;
  });
  session.post('Runtime.enable');
  let evaluated;
  session.post(
    'Runtime.evaluate',
    { expression: 'console.log("from-node-session"); 6 * 7', contextId },
    (err, result) => {
      evaluated = { error: err?.message ?? null, result: result?.result };
    }
  );
  session.disconnect();
  return { contextIdType: typeof contextId, evaluated };
}

function disconnectInCallback() {
  const session = new Session();
  session.connect();
  const events = [];
  session.post('Profiler.enable', (err) => {
    events.push(err ? `error: ${err.message}` : 'callback');
    session.disconnect();
    events.push('disconnected');
  });
  let postAfter;
  try {
    session.post('Profiler.disable');
    postAfter = 'posted';
  } catch (e) {
    postAfter = e.code;
  }
  // A second disconnect is a no-op.
  session.disconnect();
  return { events, postAfter };
}

export default {
  fetch(request) {
    const url = new URL(request.url);
    switch (url.pathname) {
      case '/session-dispatch':
        return Response.json(sessionDispatch());
      case '/disconnect-in-callback':
        return Response.json(disconnectInCallback());
    }
    return new Response('Not found', { status: 404 });
  },
};
