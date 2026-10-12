// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert/strict';
import childProcess from 'node:child_process';
import { EventEmitter } from 'node:events';
import { PassThrough } from 'node:stream';
import { test } from 'node:test';
import { WorkerdServerHarness } from './server-harness.mjs';

function mockWorkerd(t, options = {}) {
  const child = new EventEmitter();
  const control = new PassThrough();
  let closed = false;
  child.once('close', () => {
    closed = true;
  });
  child.pid = 4242;
  child.stdio = [null, null, null, control];
  child.kill = (signal) => {
    if (closed) return false;
    child.emit('exit', null, signal);
    if (control.readableEnded) {
      child.emit('close', null, signal);
    } else {
      control.once('end', () => child.emit('close', null, signal));
      control.end();
    }
    return true;
  };
  t.mock.method(childProcess, 'spawn', () => {
    queueMicrotask(() => child.emit('spawn'));
    return child;
  });
  const harness = new WorkerdServerHarness({
    workerdBinary: 'mock-workerd',
    workerdConfig: 'mock-config',
    listenPortNames: ['udp'],
    ...options,
  });
  t.after(() => harness.stop());
  return { child, control, harness };
}

test('readiness messages can be split across pipe chunks', async (t) => {
  const { control, harness } = mockWorkerd(t);
  const started = harness.start();
  control.write('{"event":"listen-ins');
  control.write('pector","port":9229}\n');
  control.write('{"event":"listen","socket":"udp","port":');
  control.write('4321}\n');
  await started;
  assert.equal(await harness.getListenInspectorPort(), 9229);
  assert.equal(await harness.getListenPort('udp'), 4321);
});

test('multiple readiness messages can share one pipe chunk', async (t) => {
  const { control, harness } = mockWorkerd(t);
  const started = harness.start();
  control.write(
    '{"event":"listen-inspector","port":9229}\n' +
      '{"event":"listen","socket":"udp","port":4321}\n'
  );
  await started;
  assert.equal(await harness.getListenInspectorPort(), 9229);
  assert.equal(await harness.getListenPort('udp'), 4321);
});

test('child exit rejects readiness with the exit status', async (t) => {
  const { child, harness } = mockWorkerd(t);
  await harness.start();
  const failed = assert.rejects(
    harness.getListenPort('udp'),
    /PID 4242.*process exited \(code 17, signal null\)/
  );
  child.emit('exit', 17, null);
  child.emit('close', 17, null);
  await failed;
});

test('spawn errors reject readiness', async (t) => {
  const { child, harness } = mockWorkerd(t);
  const failed = assert.rejects(harness.start(), /spawn ENOENT/);
  child.emit('error', new Error('spawn ENOENT'));
  child.emit('close', -2, null);
  await failed;
});

test('control EOF rejects readiness even if the child is still alive', async (t) => {
  const { control, harness } = mockWorkerd(t);
  await harness.start();
  const failed = assert.rejects(
    harness.getListenPort('udp'),
    /control stream ended before readiness/
  );
  control.end();
  await failed;
});

test('startup timeout reports the PID, pending socket, and last message', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const { control, harness } = mockWorkerd(t, { startupTimeoutMs: 30 });
  await harness.start();
  const failed = assert.rejects(harness.getListenPort('udp'), (error) => {
    assert.match(error.message, /PID 4242/);
    assert.match(error.message, /timed out after 30ms waiting for listen:udp/);
    assert.match(
      error.message,
      /last control message: {"event":"listen-inspector","port":9229}/
    );
    return true;
  });
  control.write('{"event":"listen-inspector","port":9229}\n');
  await Promise.resolve();
  t.mock.timers.tick(30);
  await failed;
});

test('malformed control messages reject readiness', async (t) => {
  const { control, harness } = mockWorkerd(t);
  await harness.start();
  const failed = assert.rejects(
    harness.getListenPort('udp'),
    /invalid control message/
  );
  control.write('not JSON\n');
  await failed;
});
