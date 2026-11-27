// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Pins how workerd's DevTools integration behaves: the HTTP endpoints, pausing and stepping,
// session lifetime, the CDP commands and events workerd handles itself, and node:inspector
// sessions next to a DevTools one. Messages are compared as parsed JSON values, not as bytes.
import { test as nodeTest } from 'node:test';
import assert from 'node:assert';
import http from 'node:http';
import { workerd } from './inspector-test-harness.mjs';
import { CdpClient, enableRuntime } from './cdp-client.mjs';

// node:test's test(), with a time limit, so that a test that gets stuck fails on its own and is
// named. afterEach still runs, and stops workerd.
function test(name, ...rest) {
  const fn = rest.pop();
  return nodeTest(name, { timeout: 30_000, ...rest[0] }, fn);
}

async function inspectorPort() {
  return await workerd.getListenInspectorPort();
}

async function connect(target = '/main') {
  return await CdpClient.connect(await inspectorPort(), target);
}

async function request(path, socket = 'http') {
  const port = await workerd.getListenPort(socket);
  return await fetch(`http://127.0.0.1:${port}${path}`);
}

// Enables the Debugger domain and returns the script that `urlSuffix` names, as
// Debugger.scriptParsed reported it.
async function enableDebugger(client, urlSuffix = 'debugger-worker.mjs') {
  const parsed = client.waitForEvent('Debugger.scriptParsed', (params) =>
    params.url.endsWith(urlSuffix)
  );
  await client.call('Debugger.enable');
  return (await parsed).params;
}

// The 0-based line of `script` that contains `marker`.
async function findLine(client, script, marker) {
  const { scriptSource } = await client.call('Debugger.getScriptSource', {
    scriptId: script.scriptId,
  });
  const line = scriptSource.split('\n').findIndex((l) => l.includes(marker));
  assert.notStrictEqual(line, -1, `${marker} is in the script`);
  return line;
}

// Stands for a `timestamp` or `wallTime`, which withoutTimes() checks is a number.
const TIME = '<time>';

// `params` with its `timestamp` and `wallTime` numbers replaced by TIME.
function withoutTimes(params) {
  const copy = { ...params };
  for (const key of ['timestamp', 'wallTime']) {
    if (key in copy) {
      assert.strictEqual(typeof copy[key], 'number', key);
      copy[key] = TIME;
    }
  }
  return copy;
}

// Resumes each pause as it is reported, and returns the function names of each paused stack.
function resumeEveryPause(client) {
  const pauses = [];
  (async () => {
    for (;;) {
      const paused = await client.waitForEvent('Debugger.paused');
      pauses.push(paused.params.callFrames.map((f) => f.functionName));
      await client.send('Debugger.resume');
    }
  })();
  return pauses;
}

function topFrame(paused) {
  const [frame] = paused.params.callFrames;
  return {
    functionName: frame.functionName,
    lineNumber: frame.location.lineNumber,
  };
}

test('/json/version and /json/list describe workerd and its isolates', async () => {
  const port = await inspectorPort();
  const host = `127.0.0.1:${port}`;

  const version = await fetch(`http://${host}/json/version`);
  assert.strictEqual(version.status, 200);
  assert.strictEqual(version.headers.get('content-type'), 'application/json');
  assert.deepStrictEqual(await version.json(), {
    Browser: 'workerd',
    'Protocol-Version': '1.3',
  });

  const expected = ['backend', 'main', 'node-inspector'].map((id) => ({
    id,
    title: `workerd: worker ${id}`,
    type: 'node',
    description: 'workerd worker',
    webSocketDebuggerUrl: `ws://${host}/${id}`,
    devtoolsFrontendUrl: `devtools://devtools/bundled/js_app.html?experiments=true&v8only=true&ws=${host}/`,
    devtoolsFrontendUrlCompat: `devtools://devtools/bundled/inspector.html?experiments=true&v8only=true&ws=${host}/`,
    faviconUrl: 'https://workers.cloudflare.com/favicon.ico',
    url: 'https://workers.dev',
  }));
  for (const path of ['/json', '/json/list', '/json/list?for_tab']) {
    const list = await fetch(`http://${host}${path}`);
    assert.strictEqual(list.status, 200, path);
    assert.strictEqual(list.headers.get('content-type'), 'application/json');
    const targets = await list.json();
    targets.sort((a, b) => a.id.localeCompare(b.id));
    assert.deepStrictEqual(targets, expected, path);
  }
});

test('A WebSocket to an unknown isolate id gets 404', async () => {
  const port = await inspectorPort();
  const response = await new Promise((resolve, reject) => {
    const req = http.request({
      host: '127.0.0.1',
      port,
      path: '/no-such-isolate',
      headers: {
        Connection: 'Upgrade',
        Upgrade: 'websocket',
        'Sec-WebSocket-Key': 'dGhlIHNhbXBsZSBub25jZQ==',
        'Sec-WebSocket-Version': '13',
      },
    });
    req.on('response', (res) => {
      let body = '';
      res.setEncoding('utf8');
      res.on('data', (chunk) => (body += chunk));
      res.on('end', () =>
        resolve({
          status: res.statusCode,
          statusText: res.statusMessage,
          body,
        })
      );
    });
    req.on('upgrade', () => reject(new Error('the upgrade was accepted')));
    req.on('error', reject);
    req.end();
  });
  assert.deepStrictEqual(response, {
    status: 404,
    statusText: 'Unknown worker session',
    body: 'Unknown worker session',
  });
});

test('A breakpoint set by URL pauses, then stepOver, stepInto and resume', async () => {
  const client = await connect();
  const script = await enableDebugger(client);
  const breakpointLine = await findLine(client, script, '// BREAKPOINT');
  const stepOverLine = await findLine(client, script, '// STEP-OVER');
  const stepIntoLine = await findLine(client, script, '// STEP-INTO');

  const breakpoint = await client.call('Debugger.setBreakpointByUrl', {
    url: script.url,
    lineNumber: breakpointLine,
  });
  assert.strictEqual(typeof breakpoint.breakpointId, 'string');
  assert.deepStrictEqual(
    breakpoint.locations.map(({ scriptId, lineNumber }) => ({
      scriptId,
      lineNumber,
    })),
    [{ scriptId: script.scriptId, lineNumber: breakpointLine }]
  );

  const response = request('/step');

  const hit = await client.waitForEvent('Debugger.paused');
  assert.strictEqual(hit.params.reason, 'other');
  assert.deepStrictEqual(hit.params.hitBreakpoints, [breakpoint.breakpointId]);
  assert.deepStrictEqual(topFrame(hit), {
    functionName: 'stepTarget',
    lineNumber: breakpointLine,
  });

  assert.deepStrictEqual(await client.call('Debugger.stepOver'), {});
  const overResumed = await client.waitForEvent('Debugger.resumed');
  assert.deepStrictEqual(overResumed.params, {});
  const over = await client.waitForEvent('Debugger.paused');
  assert.deepStrictEqual(topFrame(over), {
    functionName: 'stepTarget',
    lineNumber: stepOverLine,
  });

  assert.deepStrictEqual(await client.call('Debugger.stepInto'), {});
  const into = await client.waitForEvent('Debugger.paused');
  assert.deepStrictEqual(topFrame(into), {
    functionName: 'stepHelper',
    lineNumber: stepIntoLine,
  });
  assert.deepStrictEqual(
    into.params.callFrames.slice(0, 2).map((frame) => frame.functionName),
    ['stepHelper', 'stepTarget']
  );

  assert.deepStrictEqual(await client.call('Debugger.resume'), {});
  const res = await response;
  assert.strictEqual(res.status, 200);
  assert.strictEqual(await res.text(), '42');

  await client.close();
});

test('A debugger statement pauses, and Debugger.evaluateOnCallFrame sees its locals', async () => {
  const client = await connect();
  await enableDebugger(client);

  const response = request('/debugger-statement');
  const paused = await client.waitForEvent('Debugger.paused');
  assert.strictEqual(paused.params.reason, 'other');
  assert.deepStrictEqual(paused.params.hitBreakpoints, []);
  assert.strictEqual(topFrame(paused).functionName, 'debuggerStatement');

  const evaluated = await client.send('Debugger.evaluateOnCallFrame', {
    callFrameId: paused.params.callFrames[0].callFrameId,
    expression: 'local + "!"',
  });
  assert.deepStrictEqual(evaluated.result, {
    result: { type: 'string', value: 'local-value!' },
  });

  const thrown = await client.call('Debugger.evaluateOnCallFrame', {
    callFrameId: paused.params.callFrames[0].callFrameId,
    expression: 'notDefinedAnywhere',
  });
  assert.strictEqual(thrown.result.subtype, 'error');
  assert.strictEqual(
    thrown.exceptionDetails.exception.className,
    'ReferenceError'
  );

  await client.call('Debugger.resume');
  const res = await response;
  assert.strictEqual(await res.text(), 'after-debugger local-value');

  await client.close();
});

// Debugger.pause while no JavaScript runs is acknowledged, but the next request runs to completion
// without pausing. (Chrome's DevTools pause on the next statement that runs.)
test(
  'GAP: Debugger.pause while JavaScript is idle pauses the next request',
  { todo: 'the next request runs without pausing' },
  async () => {
    const client = await connect();
    await enableDebugger(client);

    assert.deepStrictEqual(await client.call('Debugger.pause'), {});

    // Resume every pause, wherever it lands, so that the request finishes either way. A pause in the
    // request is reported before the request can finish.
    const pauses = resumeEveryPause(client);
    const response = await request('/idle-target');
    assert.strictEqual(await response.text(), 'idle-target');
    assert(
      pauses.some(
        (frames) => frames.includes('idleTarget') || frames.includes('fetch')
      ),
      `paused in the request: ${JSON.stringify(pauses)}`
    );

    await client.close();
  }
);

test('Disconnecting while paused resumes execution, and the request completes', async () => {
  const client = await connect();
  await enableDebugger(client);

  const response = request('/debugger-statement');
  await client.waitForEvent('Debugger.paused');
  await client.close();

  const res = await response;
  assert.strictEqual(res.status, 200);
  assert.strictEqual(await res.text(), 'after-debugger local-value');

  // The isolate still takes a new session, which sees it running.
  const next = await connect();
  const contextId = await enableRuntime(next);
  const evaluated = await next.call('Runtime.evaluate', {
    expression: '1 + 1',
    contextId,
  });
  assert.deepStrictEqual(evaluated, {
    result: { type: 'number', value: 2, description: '2' },
  });
  await next.close();
});

test('A new session boots the previous one', async () => {
  const first = await connect();
  await first.call('Runtime.getHeapUsage');

  const second = connect();
  assert.deepStrictEqual(await first.closed, {
    code: 1000,
    reason: 'client closed connection',
  });
  await (await second).close();
});

// The booted session's teardown disconnects whichever session is current, which by then is the
// new one: ~InspectorChannelImpl() checks only that `currentInspectorSession` is set, not that it
// is itself. So the new session is closed too, with the same 1000 "client closed connection",
// before it receives anything.
test(
  'GAP: the session that boots the previous one stays attached',
  { todo: "the old session's teardown closes the new one too" },
  async () => {
    const first = await connect();
    await first.call('Runtime.getHeapUsage');

    const second = await connect();
    await first.closed;

    const usage = await second.call('Runtime.getHeapUsage');
    assert(usage.usedSize > 0, JSON.stringify(usage));
    await second.close();
  }
);

test('console messages reach Runtime.consoleAPICalled', async () => {
  const client = await connect();
  const contextId = await enableRuntime(client);

  const res = await request('/console');
  assert.strictEqual(await res.text(), 'ok');

  const log = await client.waitForEvent('Runtime.consoleAPICalled');
  assert.strictEqual(log.params.type, 'log');
  assert.strictEqual(log.params.executionContextId, contextId);
  assert.deepStrictEqual(log.params.args, [
    { type: 'string', value: 'console-message' },
    { type: 'number', value: 42, description: '42' },
  ]);
  assert.strictEqual(log.params.stackTrace.callFrames[0].functionName, 'fetch');

  const warn = await client.waitForEvent('Runtime.consoleAPICalled');
  assert.strictEqual(warn.params.type, 'warning');
  assert.deepStrictEqual(warn.params.args, [
    { type: 'string', value: 'console-warning' },
  ]);

  await client.close();
});

test("workerd's own warnings, logged before any session, are delivered when one attaches", async () => {
  const res = await request('/warn-header');
  assert.strictEqual(await res.text(), 'ok');

  // Not enabling Runtime: workerd sends the queued notifications as soon as the session attaches.
  const client = await connect();
  const warning = await client.waitForEvent('Runtime.consoleAPICalled');
  assert.deepStrictEqual(Object.keys(warning.params).sort(), [
    'args',
    'executionContextId',
    'stackTrace',
    'timestamp',
    'type',
  ]);
  assert.strictEqual(warning.params.type, 'warning');
  assert.strictEqual(warning.params.args.length, 1);
  assert.strictEqual(warning.params.args[0].type, 'string');
  assert.match(
    warning.params.args[0].value,
    /^A header value for "x-non-ascii" contains non-ASCII characters/
  );
  assert.strictEqual(typeof warning.params.executionContextId, 'number');
  assert.strictEqual(typeof warning.params.timestamp, 'number');
  const [frame] = warning.params.stackTrace.callFrames;
  assert.deepStrictEqual(Object.keys(frame).sort(), [
    'columnNumber',
    'functionName',
    'lineNumber',
    'scriptId',
    'url',
  ]);
  assert.strictEqual(frame.functionName, 'fetch');
  assert(frame.url.endsWith('debugger-worker.mjs'), frame.url);

  // Delivered once: the queue is empty for the next session.
  await client.close();
  const next = await connect();
  await next.call('Runtime.getHeapUsage');
  assert.deepStrictEqual(
    next.messages.filter((m) => m.method === 'Runtime.consoleAPICalled'),
    []
  );
  await next.close();
});

test('Network events report a subrequest; Network.getResponseBody is unsupported', async () => {
  const client = await connect();
  const enabled = await client.send('Network.enable');
  assert.deepStrictEqual(enabled, {
    id: enabled.id,
    method: 'Network.enable',
    result: {},
  });

  const res = await request('/subrequest');
  assert.strictEqual(await res.text(), '{"hello":"backend"}');

  // loadingFinished is sent once the response body's stream is gone, after the body was read.
  await client.waitForEvent('Network.loadingFinished');
  const events = client.messages
    .filter((m) => m.id === undefined && m.method.startsWith('Network.'))
    .map(({ method, params }) => ({ method, params: withoutTimes(params) }));
  const requestId = events[0].params.requestId;
  assert.strictEqual(typeof requestId, 'string');
  assert.deepStrictEqual(events, [
    {
      method: 'Network.requestWillBeSent',
      params: {
        requestId,
        loaderId: '',
        request: {
          url: 'http://backend.test/data?x=1',
          method: 'GET',
          headers: { 'x-request': 'value' },
          mixedContentType: 'blockable',
          initialPriority: 'VeryLow',
          referrerPolicy: 'unsafe-url',
          isLinkPreload: false,
        },
        timestamp: TIME,
        wallTime: TIME,
        // The subrequest starts after the fetch() call returns, so no JavaScript is on the stack.
        initiator: { type: 'script', stack: { callFrames: [] }, lineNumber: 0 },
        type: 'Fetch',
      },
    },
    {
      method: 'Network.responseReceived',
      params: {
        requestId,
        timestamp: TIME,
        type: 'XHR',
        response: {
          status: 200,
          statusText: 'OK',
          headers: { 'Content-Type': 'application/json', 'x-backend': 'yes' },
          mimeType: 'application/json',
          connectionReused: false,
          connectionId: 0,
          remotePort: 0,
          fromDiskCache: false,
          fromServiceWorker: false,
          encodedDataLength: 0,
          protocol: 'http/1.1',
          securityState: 'unknown',
        },
      },
    },
    {
      method: 'Network.dataReceived',
      params: {
        requestId,
        timestamp: TIME,
        dataLength: 19,
        encodedDataLength: 19,
      },
    },
    {
      method: 'Network.loadingFinished',
      params: {
        requestId,
        timestamp: TIME,
        encodedDataLength: 19,
        cfResponse: {
          body: Buffer.from('{"hello":"backend"}').toString('base64'),
          base64Encoded: true,
        },
      },
    },
  ]);

  const body = await client.send('Network.getResponseBody', { requestId });
  assert.deepStrictEqual(body, {
    id: body.id,
    method: 'Network.getResponseBody',
    error: {
      code: -32600,
      message: 'Network.getResponseBody is not supported in this fork',
    },
  });

  const disabled = await client.send('Network.disable');
  assert.deepStrictEqual(disabled, {
    id: disabled.id,
    method: 'Network.disable',
    result: {},
  });
  await client.close();
});

test('HeapProfiler.takeHeapSnapshot sends chunks that parse as one JSON document', async () => {
  const client = await connect();
  const response = await client.send('HeapProfiler.takeHeapSnapshot', {
    reportProgress: true,
  });
  // workerd answers with the command's own params rather than an empty result.
  assert.deepStrictEqual(response, {
    id: response.id,
    method: 'HeapProfiler.takeHeapSnapshot',
    params: {
      reportProgress: true,
      captureNumericValue: false,
      exposeInternals: false,
    },
  });

  // Every event is sent before the response.
  const before = client.messages.slice(0, client.messages.indexOf(response));
  const chunks = before
    .filter((m) => m.method === 'HeapProfiler.addHeapSnapshotChunk')
    .map((m) => m.params.chunk);
  const progress = before
    .filter((m) => m.method === 'HeapProfiler.reportHeapSnapshotProgress')
    .map((m) => m.params);
  assert(chunks.length > 0);
  assert(progress.length > 0);
  const last = progress[progress.length - 1];
  assert.strictEqual(last.done, last.total);
  assert.strictEqual(last.finished, true);
  for (const p of progress.slice(0, -1)) {
    assert.deepStrictEqual(Object.keys(p).sort(), [
      'done',
      'finished',
      'total',
    ]);
    assert.strictEqual(p.finished, false);
  }

  const snapshot = JSON.parse(chunks.join(''));
  assert.strictEqual(typeof snapshot.snapshot.node_count, 'number');
  assert.strictEqual(typeof snapshot.snapshot.edge_count, 'number');
  assert(snapshot.snapshot.edge_count > 0);
  const nodeFields = snapshot.snapshot.meta.node_fields.length;
  assert.strictEqual(
    snapshot.nodes.length,
    snapshot.snapshot.node_count * nodeFields
  );
  assert(snapshot.strings.includes('stepTarget'));

  await client.close();
});

test('A CDP message over 1 MB is accepted', async () => {
  const client = await connect();
  const contextId = await enableRuntime(client);
  const length = 1536 * 1024;
  const evaluated = await client.call('Runtime.evaluate', {
    expression: `"${'a'.repeat(length)}".length`,
    contextId,
  });
  assert.deepStrictEqual(evaluated, {
    result: { type: 'number', value: length, description: String(length) },
  });
  await client.close();
});

// A pause that starts inside a dispatch: handleDispatchProtocolMessage() holds the channel's
// `state` mutex while it dispatches, and dispatchOneMessageDuringPause() takes it again for the
// first message that arrives during the pause. Debugger.paused is sent, but Debugger.resume
// deadlocks the isolate thread; workerd then ignores SIGTERM, so this cannot run.
test(
  'GAP: Runtime.evaluate of a debugger statement pauses, then resumes',
  { skip: 'deadlocks workerd: the channel state mutex is taken twice' },
  async () => {
    const client = await connect();
    await enableDebugger(client);
    const contextId = await enableRuntime(client);

    const evaluated = client.send('Runtime.evaluate', {
      expression: 'debugger; "evaluated"',
      contextId,
    });
    const paused = await client.waitForEvent('Debugger.paused');
    assert.strictEqual(paused.params.reason, 'other');
    await client.call('Debugger.resume');
    assert.deepStrictEqual((await evaluated).result, {
      result: { type: 'string', value: 'evaluated' },
    });

    await client.close();
  }
);

test('A node:inspector session dispatches while a DevTools session is attached', async () => {
  const client = await connect('/node-inspector');
  const contextId = await enableRuntime(client);

  const res = await request('/session-dispatch', 'node');
  assert.deepStrictEqual(await res.json(), {
    contextIdType: 'number',
    evaluated: {
      error: null,
      result: { type: 'number', value: 42, description: '42' },
    },
  });

  const log = await client.waitForEvent('Runtime.consoleAPICalled');
  assert.strictEqual(log.params.executionContextId, contextId);
  assert.deepStrictEqual(log.params.args, [
    { type: 'string', value: 'from-node-session' },
  ]);

  // The DevTools session is unaffected by the other session's disconnect.
  const evaluated = await client.call('Runtime.evaluate', {
    expression: '2 * 3',
    contextId,
  });
  assert.deepStrictEqual(evaluated, {
    result: { type: 'number', value: 6, description: '6' },
  });
  await client.close();
});

test('A node:inspector session may disconnect from inside its own callback', async () => {
  const client = await connect('/node-inspector');
  const contextId = await enableRuntime(client);

  const res = await request('/disconnect-in-callback', 'node');
  assert.deepStrictEqual(await res.json(), {
    events: ['callback', 'disconnected'],
    postAfter: 'ERR_INSPECTOR_NOT_CONNECTED',
  });

  const evaluated = await client.call('Runtime.evaluate', {
    expression: '"still attached"',
    contextId,
  });
  assert.deepStrictEqual(evaluated, {
    result: { type: 'string', value: 'still attached' },
  });
  await client.close();
});

// Messages are dispatched only when JavaScript
// yields the isolate lock, and nothing interrupts running JavaScript, so DevTools' Pause button
// does nothing during a busy loop. Debugger.pause is dispatched once the request returns, and then,
// as when JavaScript is idle, nothing pauses.
test(
  'GAP: Debugger.pause during a busy loop pauses inside it',
  { todo: 'nothing interrupts running JavaScript' },
  async () => {
    const client = await connect();
    await enableDebugger(client);
    await enableRuntime(client);

    const response = request('/busy');
    await client.waitForEvent(
      'Runtime.consoleAPICalled',
      (params) => params.args[0]?.value === 'busy-start'
    );
    const pauseResponse = client.send('Debugger.pause');

    const pauses = resumeEveryPause(client);
    assert.strictEqual(await (await response).text(), 'busy-done');
    assert.deepStrictEqual(await pauseResponse.then((m) => m.result), {});
    assert(
      pauses.some((frames) => frames[0] === 'busyLoop'),
      `paused in the busy loop: ${JSON.stringify(pauses)}`
    );
    await client.close();
  }
);

// A pause while no request's IoContext is current.
// workerd evaluates a dynamically imported module outside the importing request's IoContext, as it
// does a Worker's top level at startup (where no session can attach first). Debugger.paused is sent
// (one frame, with an empty function name and URL), but the first message that arrives during the
// pause makes dispatchOneMessageDuringPause() call IoContext::current(), which throws
// JsExceptionThrown out of V8's runMessageLoopOnPause(): workerd aborts with "Fatal uncaught
// std::exception: Uncaught JsExceptionThrown".
test(
  'GAP: a debugger statement in module top-level code pauses and resumes',
  { skip: 'crashes workerd: IoContext::current() throws during the pause' },
  async () => {
    const client = await connect();
    await enableDebugger(client);

    const response = request('/top-level-debugger');
    await client.waitForEvent('Debugger.paused');
    await client.call('Debugger.resume');
    assert.strictEqual(await (await response).text(), 'top-level');
    await client.close();
  }
);

// While the isolate thread is paused it holds the isolate lock, and attachInspector() takes that
// lock synchronously on the inspector service's thread, which also carries every session's
// messages. So a session connecting during a pause deadlocks both threads: the new WebSocket is
// accepted, the old session is never closed, and workerd ignores SIGTERM.
test(
  'GAP: a new session connects while the previous one is paused',
  {
    skip: 'deadlocks workerd: attaching takes the isolate lock that the pause holds',
  },
  async () => {
    const first = await connect();
    await enableDebugger(first);

    const response = request('/debugger-statement');
    await first.waitForEvent('Debugger.paused');

    const second = await connect();
    await first.closed;
    assert.strictEqual(
      await (await response).text(),
      'after-debugger local-value'
    );
    await second.close();
  }
);
