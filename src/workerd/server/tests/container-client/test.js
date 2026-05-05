// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';
import assert from 'node:assert';
import { scheduler } from 'node:timers/promises';

// 5s timeout for some of the requests going to the container.
// We can get to have a stack trace with an
// abort signal.
const DEFAULT_TIMEOUT_DURATION = 10_000;

// Use a unique DO name per test invocation because different test flavors may
// run concurrently, and this avoids them accidentally sharing the same object.
function getRandomDurableObjectName(name) {
  return `${name}-${crypto.randomUUID()}`;
}

// **IMPORTANT NOTE**
//
// When writing a test, don't forget to call waitUntilContainerIsHealthy
// before testing the behaviour with your container.
//
// Don't forget to call monitor() after calling start(), as there
// is an issue with not calling monitor() in Durable Objects where
// we might lose track of the container lifetime.
//

export class DurableObjectExample extends DurableObject {
  async testExitCode() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }
    assert.strictEqual(container.running, false);

    // Start container with invalid entrypoint
    {
      container.start({
        entrypoint: ['node', 'nonexistant.js'],
      });

      let exitCode = undefined;
      await container.monitor().catch((err) => {
        exitCode = err.exitCode;
      });

      assert.strictEqual(typeof exitCode, 'number');
      assert.notEqual(0, exitCode);
    }

    // Start container with valid entrypoint and stop it
    {
      container.start();

      await scheduler.wait(500);

      let exitCode = undefined;
      const monitor = container.monitor().catch((err) => {
        exitCode = err.exitCode;
      });
      await container.destroy();
      await monitor;

      assert.strictEqual(typeof exitCode, 'number');
      assert.equal(137, exitCode);
    }
  }

  async testBasics() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});

      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    // Start container with valid configuration
    container.start({
      env: { A: 'B', C: 'D', L: 'F' },
      enableInternet: true,
    });

    const monitor = container.monitor().catch((_err) => {});

    await this.waitUntilContainerIsHealthy();

    await container.destroy();

    await monitor;
    assert.strictEqual(container.running, false);
  }

  async testExec() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    container.start({
      env: { EXEC_BASE: 'from-start' },
      enableInternet: true,
    });

    const monitor = container.monitor().catch((_err) => {});
    const textEncoder = new TextEncoder();
    const textDecoder = new TextDecoder();
    const decode = (buffer) => textDecoder.decode(buffer);
    const countStreamBytes = async (stream) => {
      assert.ok(stream);

      const reader = stream.getReader();
      let total = 0;
      try {
        for (;;) {
          const { value, done } = await reader.read();
          if (done) {
            return total;
          }

          total += value.byteLength;
        }
      } finally {
        reader.releaseLock();
      }
    };

    await this.waitUntilContainerIsHealthy();

    // 1. Read stdout directly as a stream.
    {
      const proc = await container.exec(['cat', '/etc/hostname']);
      assert.ok(proc.pid > 0);
      const stdout = await new Response(proc.stdout).text();
      assert.ok(stdout.trim().length > 0);
      assert.strictEqual(await proc.exitCode, 0);
    }

    // 2. Create a file from a ReadableStream stdin using tee.
    {
      const content = '{"hello":"world","kind":"stream"}\n';
      const proc = await container.exec(['tee', '/tmp/exec-stream.json'], {
        stdin: new ReadableStream({
          start(controller) {
            controller.enqueue(textEncoder.encode(content));
            controller.close();
          },
        }),
        stdout: 'ignore',
      });
      assert.strictEqual(await proc.exitCode, 0);

      const verify = await (
        await container.exec(['cat', '/tmp/exec-stream.json'])
      ).output();

      assert.strictEqual(decode(verify.stdout), content);
      assert.strictEqual(verify.exitCode, 0);
    }

    // 3. Feed stdin interactively through the exposed WritableStream.
    {
      const proc = await container.exec(
        ['sh', '-lc', 'cat > /tmp/exec-pipe.txt'],
        {
          stdin: 'pipe',
          stdout: 'ignore',
        }
      );
      assert.ok(proc.stdin);

      const writer = proc.stdin.getWriter();
      await writer.write(textEncoder.encode('alpha\n'));
      await writer.write(textEncoder.encode('beta\n'));
      await writer.close();

      assert.strictEqual(await proc.exitCode, 0);

      const verify = await (
        await container.exec(['cat', '/tmp/exec-pipe.txt'])
      ).output();
      assert.strictEqual(decode(verify.stdout), 'alpha\nbeta\n');
    }

    // 4. Override working directory for commands that rely on relative paths.
    {
      const proc = await container.exec(['pwd'], { cwd: '/tmp' });
      const output = await proc.output();
      assert.strictEqual(decode(output.stdout).trim(), '/tmp');
      assert.strictEqual(output.exitCode, 0);
    }

    // 5. Merge container env with per-exec overrides.
    {
      const proc = await container.exec(
        ['sh', '-lc', 'printf "%s|%s" "$EXEC_BASE" "$EXEC_EXTRA"'],
        {
          env: {
            EXEC_BASE: 'overridden',
            EXEC_EXTRA: 'per-exec',
          },
        }
      );
      const output = await proc.output();
      assert.strictEqual(decode(output.stdout), 'overridden|per-exec');
      assert.strictEqual(output.exitCode, 0);
    }

    // 6. Capture stdout and stderr separately.
    {
      const proc = await container.exec([
        'sh',
        '-lc',
        'printf "out"; printf "err" >&2',
      ]);
      const output = await proc.output();
      assert.strictEqual(decode(output.stdout), 'out');
      assert.strictEqual(decode(output.stderr), 'err');
      assert.strictEqual(output.exitCode, 0);
    }

    // 7. Combine stderr into stdout for shell-style command output.
    {
      const proc = await container.exec(
        ['sh', '-lc', 'printf "out"; printf "err" >&2'],
        { stderr: 'combined' }
      );
      const output = await proc.output();
      const stdout = decode(output.stdout);
      if (stdout !== 'outerr') {
        assert.strictEqual(decode(output.stdout), 'errout');
      }

      assert.strictEqual(decode(output.stderr), '');
      assert.strictEqual(output.exitCode, 0);
    }

    // 8. Ignore stdout when only success/failure matters.
    {
      const proc = await container.exec(['sh', '-lc', 'printf "ignore-me"'], {
        stdout: 'ignore',
      });
      const output = await proc.output();
      assert.strictEqual(decode(output.stdout), '');
      assert.strictEqual(decode(output.stderr), '');
      assert.strictEqual(output.exitCode, 0);
    }

    // 9. Preserve stderr and non-zero exit codes for failures.
    {
      const proc = await container.exec([
        'sh',
        '-lc',
        'printf "boom" >&2; exit 7',
      ]);
      const output = await proc.output();
      assert.strictEqual(decode(output.stdout), '');
      assert.strictEqual(decode(output.stderr), 'boom');
      assert.strictEqual(output.exitCode, 7);
    }

    // 10. Stream-consume large stdout and stderr payloads concurrently without buffering them in
    // JS memory.
    {
      const expectedBytes = 64 * 1024 * 1024;
      const proc = await container.exec([
        'sh',
        '-lc',
        `head -c ${expectedBytes} /dev/zero & head -c ${expectedBytes} /dev/zero >&2 & wait`,
      ]);

      const [stdoutBytes, stderrBytes, exitCode] = await Promise.all([
        countStreamBytes(proc.stdout),
        countStreamBytes(proc.stderr),
        proc.exitCode,
      ]);

      assert.strictEqual(stdoutBytes, expectedBytes);
      assert.strictEqual(stderrBytes, expectedBytes);
      assert.strictEqual(exitCode, 0);
    }

    // 11. Check we throw an error when calling output() after reading from stdout
    {
      const proc = await container.exec(['echo', 'hello']);
      await proc.stdout.getReader().read();
      assert.rejects(() => proc.output(), {
        name: 'TypeError',
        message:
          'Cannot call output() after stdout has started being consumed.',
      });
    }

    // 12. Make sure Stdin EOF's by default if not set
    {
      await container.exec(['cat']).then((p) => p.output());
    }

    await container.destroy();
    await monitor;
    assert.strictEqual(container.running, false);
  }

  async testSetInactivityTimeout(timeout) {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }
    assert.strictEqual(container.running, false);

    container.start();

    assert.strictEqual(container.running, true);

    // Wait for container to be running
    await scheduler.wait(500);

    try {
      await container.setInactivityTimeout(0);
    } catch (err) {
      assert.strictEqual(err.name, 'TypeError');
      assert.match(
        err.message,
        /setInactivityTimeout\(\) cannot be called with a durationMs <= 0/
      );
    }

    if (timeout > 0) {
      await container.setInactivityTimeout(timeout);
    }
  }

  async start() {
    assert.strictEqual(this.ctx.container.running, false);
    this.ctx.container.start();
    assert.strictEqual(this.ctx.container.running, true);

    // Wait for container to be running
    await scheduler.wait(500);
  }

  // Assert that the container is running
  async expectRunning(running) {
    assert.strictEqual(this.ctx.container.running, running);
    await this.ctx.container.destroy();
  }

  async abort() {
    await this.ctx.storage.put('aborted', true);
    await this.ctx.storage.sync();
    this.ctx.abort();
  }

  async alarm() {
    const alarmValue = (await this.ctx.storage.get('alarm')) ?? 0;

    const aborted = await this.ctx.storage.get('aborted');
    assert.strictEqual(!!this.ctx.container, true);
    if (aborted) {
      await this.ctx.storage.put('aborted-confirmed', true);
    }

    await this.ctx.storage.put('alarm', alarmValue + 1);
  }

  async getAlarmIndex() {
    return (await this.ctx.storage.get('alarm')) ?? 0;
  }

  async startAlarm(start, ms) {
    if (start && !this.ctx.container.running) {
      this.ctx.container.start();
    }
    await this.ctx.storage.setAlarm(Date.now() + ms);
  }

  async checkAlarmAbortConfirmation() {
    const abortConfirmation = await this.ctx.storage.get('aborted-confirmed');
    if (!abortConfirmation) {
      throw new Error(
        `Abort confirmation did not get inserted: ${abortConfirmation}`
      );
    }
  }

  async testWs() {
    const { container } = this.ctx;

    if (!container.running) {
      container.start({
        env: { WS_ENABLED: 'true' },
        enableInternet: true,
      });
    }

    await this.waitUntilContainerIsHealthy();

    const res = await container.getTcpPort(8080).fetch('http://foo/ws', {
      headers: {
        Upgrade: 'websocket',
        Connection: 'Upgrade',
        'Sec-WebSocket-Key': 'x3JJHMbDL1EzLkh9GBhXDw==',
        'Sec-WebSocket-Version': '13',
      },
      signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
    });

    // Should get WebSocket upgrade response
    assert.strictEqual(res.status, 101);
    assert.strictEqual(res.headers.get('upgrade'), 'websocket');
    assert.strictEqual(!!res.webSocket, true);

    // Test basic WebSocket communication
    const ws = res.webSocket;
    ws.accept();

    // Listen for response
    const messagePromise = new Promise((resolve) => {
      ws.addEventListener(
        'message',
        (event) => {
          resolve(event.data);
        },
        { once: true }
      );
    });

    // Send a test message
    ws.send('Hello WebSocket!');

    assert.strictEqual(await messagePromise, 'Echo: Hello WebSocket!');

    ws.close();
    await container.destroy();
  }

  getStatus() {
    return this.ctx.container.running;
  }

  async waitUntilContainerIsHealthy() {
    const container = this.ctx.container;
    {
      let resp;
      // The retry count here is arbitrary. Can increase it if necessary.
      const maxRetries = 15;
      for (let i = 1; i <= maxRetries; i++) {
        try {
          resp = await container.getTcpPort(8080).fetch('http://foo/bar/baz', {
            method: 'POST',
            body: 'hello',
            signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
          });
          break;
        } catch (e) {
          if (!e.message.includes('Container is not listening to port 8080')) {
            console.error(
              'Error querying getTcpPort().fetch() that is not related to the container not listening yet',
              e.message
            );

            throw e;
          }

          if (i === maxRetries) {
            console.error(
              `Failed to connect to container ${container.id}. Retried ${i} times`
            );
            throw e;
          }

          await scheduler.wait(500);
        }
      }

      assert.equal(resp.status, 200);
      assert.equal(resp.statusText, 'OK');
      assert.strictEqual(await resp.text(), 'Hello World!');
    }
  }

  async fetchIntercept(host) {
    return await this.ctx.container
      .getTcpPort(8080)
      .fetch('http://foo/intercept', {
        headers: { 'x-host': host },
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
  }

  async expectIntercept(host, expectedStatus, expectedBody) {
    const response = await this.fetchIntercept(host);
    assert.equal(response.status, expectedStatus);
    assert.equal(await response.text(), expectedBody);
  }

  fetchHttpsIntercept(host) {
    return this.ctx.container
      .getTcpPort(8080)
      .fetch('http://foo/intercept-https', {
        headers: { 'x-host': host },
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
  }

  async expectHttpsIntercept(host, expectedStatus, expectedBody) {
    const response = await this.fetchHttpsIntercept(host);
    assert.equal(response.status, expectedStatus);
    assert.equal(await response.text(), expectedBody);
  }

  async testPortNotListening() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start();
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await assert.rejects(
      container.getTcpPort(8081).fetch('http://foo/bar', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      }),
      /Container is not listening to port 8081/
    );

    await container.destroy();
    await monitor;
  }

  async testLabels() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    const labels = {
      team: 'workers',
      environment: 'testing',
      'my.label/key': 'value',
      'workerd-foo': 'bar',
      kv: 'a=b=c',
      emoji: '🧪',
    };
    container.start({ enableInternet: true, labels });

    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    assert.strictEqual(container.running, true);

    const info = await container.inspect();
    assert.deepStrictEqual(info.labels, labels);

    await container.destroy();
    await monitor;
    assert.strictEqual(container.running, false);
  }

  async testInspectBeforeStart() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    const info = await container.inspect();
    assert.strictEqual(info, null);
  }

  async testInspectEmptyLabels() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const info = await container.inspect();
    assert.deepStrictEqual(info.labels, {});

    await container.destroy();
    await monitor;
  }

  async testInspectAfterDestroy() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    container.start({
      enableInternet: true,
      labels: { foo: 'bar' },
    });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();
    await container.destroy();
    await monitor;

    const info = await container.inspect();
    assert.strictEqual(info, null);
  }

  async testLabelValidation() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    // Empty label name
    assert.throws(() => container.start({ labels: { '': 'value' } }), {
      message: /Label names cannot be empty/,
    });

    // Label name with control character
    assert.throws(
      () => container.start({ labels: { 'bad\x01name': 'value' } }),
      { message: /Label names cannot contain control characters \(index 0\)/ }
    );

    // Label value with control character
    assert.throws(() => container.start({ labels: { name: 'bad\x01value' } }), {
      message: /Label values cannot contain control characters \(index 0\)/,
    });
  }

  async testPidNamespace() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    container.start({
      enableInternet: true,
    });

    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const resp = await container
      .getTcpPort(8080)
      .fetch('http://foo/pid-namespace', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    assert.equal(resp.status, 200);
    const data = await resp.json();

    await container.destroy();
    await monitor;
    assert.strictEqual(container.running, false);

    return data;
  }

  async testSetEgressHttpWithInternet() {
    const container = this.ctx.container;
    if (container.running) {
      let monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });

    await this.waitUntilContainerIsHealthy();

    await container.interceptOutboundHttp(
      'googlefakedomain.com',
      this.ctx.exports.TestService({ props: { id: 2 } })
    );

    await this.expectIntercept(
      'googlefakedomain.com',
      200,
      'hello binding: 2 http://googlefakedomain.com/'
    );

    await this.expectIntercept(
      'googlefakedomainother.com',
      500,
      'googlefakedomainother.com fetch failed'
    );

    await container.interceptAllOutboundHttp(
      this.ctx.exports.TestService({ props: { id: 5 } })
    );

    await this.expectIntercept(
      'google.com',
      200,
      'hello binding: 5 http://google.com/'
    );
  }

  async testSetEgressHttpNoInternet() {
    const container = this.ctx.container;

    if (!container.running) container.start();

    // wait for container to be available
    await this.waitUntilContainerIsHealthy();

    await container.interceptOutboundHttp(
      'google.com',
      this.ctx.exports.TestService({ props: { id: 2 } })
    );

    await this.expectIntercept(
      'google.com',
      200,
      'hello binding: 2 http://google.com/'
    );

    // This should fail as there is no hostname that matches it.
    await this.expectIntercept('google2.com', 500, 'google2.com fetch failed');

    await container.interceptOutboundHttp(
      'google2.com',
      this.ctx.exports.TestService({ props: { id: 4 } })
    );

    await this.expectIntercept(
      'google.com',
      200,
      'hello binding: 2 http://google.com/'
    );
    await this.expectIntercept(
      'google2.com',
      200,
      'hello binding: 4 http://google2.com/'
    );

    // From now on, all hostnames resolve to Workerd.
    await container.interceptAllOutboundHttp(
      this.ctx.exports.TestService({ props: { id: 6 } })
    );

    await this.expectIntercept(
      'google.com',
      200,
      'hello binding: 2 http://google.com/'
    );

    await this.expectIntercept(
      'google2.com',
      200,
      'hello binding: 4 http://google2.com/'
    );

    await this.expectIntercept(
      'google3.com',
      200,
      'hello binding: 6 http://google3.com/'
    );

    await this.expectIntercept(
      '1.1.1.1',
      200,
      'hello binding: 6 http://1.1.1.1/'
    );

    await this.expectIntercept('1.1.1.1:90', 500, '1.1.1.1:90 fetch failed');
    await this.expectIntercept(
      'google.com:9000',
      500,
      'google.com:9000 fetch failed'
    );
  }

  async createSnapshotForTransfer() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const writeResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/cross-do.txt', {
        method: 'POST',
        body: 'cross-do-snapshot',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(writeResp.status, 200);

    const snapshot = await container.snapshotDirectory({
      dir: '/app/data',
      name: 'cross-do-snapshot',
    });

    await container.destroy();
    await monitor;

    return snapshot;
  }

  async restoreTransferredSnapshot(snapshot) {
    assert.ok(snapshot.id, 'snapshot must have a non-empty id');
    assert.ok(snapshot.size > 0, 'snapshot must have a positive size');
    assert.strictEqual(snapshot.dir, '/app/data');

    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({
      enableInternet: true,
      directorySnapshots: [{ snapshot }],
    });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const readResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/cross-do.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(readResp.status, 200);
    assert.strictEqual(await readResp.text(), 'cross-do-snapshot');

    await container.destroy();
    await monitor;
  }

  async createContainerSnapshotForTransfer() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const writeResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/full-cross-do.txt', {
        method: 'POST',
        body: 'cross-do-container-snapshot',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(writeResp.status, 200);

    const snapshot = await container.snapshotContainer({
      name: 'cross-do-container-snapshot',
    });

    await container.destroy();
    await monitor;

    return snapshot;
  }

  async restoreTransferredContainerSnapshot(snapshot) {
    assert.ok(snapshot.id, 'snapshot must have a non-empty id');
    assert.ok(snapshot.size > 0, 'snapshot must have a positive size');

    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({
      enableInternet: true,
      containerSnapshot: snapshot,
    });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const readResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/full-cross-do.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(readResp.status, 200);
    assert.strictEqual(await readResp.text(), 'cross-do-container-snapshot');

    await container.destroy();
    await monitor;
  }

  async testSetEgressHttp() {
    const container = this.ctx.container;

    // Set up egress TCP mapping to route requests to the binding
    // We can configure this even before the container starts.
    await container.interceptOutboundHttp(
      '1.2.3.4',
      this.ctx.exports.TestService({ props: { id: 1234 } })
    );

    if (!container.running) container.start();

    // Keep container alive after abort();
    container.monitor().catch((err) => {
      console.error('Container exited with an error:', err.message);
    });

    // wait for container to be available
    await this.waitUntilContainerIsHealthy();

    // Set up egress TCP mapping to route requests to the binding
    // This registers the binding's channel token with the container runtime
    await container.interceptOutboundHttp(
      '11.0.0.1:9999',
      this.ctx.exports.TestService({ props: { id: 1 } })
    );

    await container.interceptOutboundHttp(
      '11.0.0.2:9999',
      this.ctx.exports.TestService({ props: { id: 2 } })
    );

    // we catch all http requests to port 80
    await container.interceptAllOutboundHttp(
      this.ctx.exports.TestService({ props: { id: 3 } })
    );

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '1.2.3.4:80' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 1234 http://1.2.3.4/'
      );
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '11.0.0.1:9999' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 1 http://11.0.0.1:9999/'
      );
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '11.0.0.2:9999' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 2 http://11.0.0.2:9999/'
      );
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '15.0.0.2:80' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(await response.text(), 'hello binding: 3 http://15.0.0.2/');
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '[111::]:80' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(await response.text(), 'hello binding: 3 http://[111::]/');
    }

    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': 'google.com/hello/world' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 3 http://google.com/hello/world'
      );
    }

    // test we can set another TestService
    await container.interceptAllOutboundHttp(
      this.ctx.exports.TestService({ props: { id: 1212 } })
    );

    {
      // We preserved the order...
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '11.0.0.2:9999' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 2 http://11.0.0.2:9999/'
      );
    }

    {
      // and we updated the id, even for existing connections
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '15.0.0.2:80' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 1212 http://15.0.0.2/'
      );
    }

    {
      // and we updated the id for new connections
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept', {
          headers: { 'x-host': '15.0.0.55:80' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      assert.equal(
        await response.text(),
        'hello binding: 1212 http://15.0.0.55/'
      );
    }
  }

  async testSetEgressHttps() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    await container.interceptOutboundHttps(
      'example.com:443',
      this.ctx.exports.TestService({ props: { id: 1000 } })
    );

    container.start({
      env: {
        NODE_EXTRA_CA_CERTS:
          '/etc/cloudflare/certs/cloudflare-containers-ca.crt',
      },
    });

    container.monitor().catch((err) => {
      console.error('Container exited with an error:', err.message);
    });

    await this.waitUntilContainerIsHealthy();

    await container.interceptOutboundHttps(
      '*.cloudflare.com:443',
      this.ctx.exports.TestService({ props: { id: 2000 } })
    );

    await container.interceptOutboundHttps(
      '*',
      this.ctx.exports.TestService({ props: { id: 3000 } })
    );

    await this.expectHttpsIntercept(
      'example.com',
      200,
      'hello binding: 1000 https://example.com/'
    );

    await this.expectHttpsIntercept(
      'www.cloudflare.com',
      200,
      'hello binding: 2000 https://www.cloudflare.com/'
    );

    await this.expectHttpsIntercept(
      'google.com',
      200,
      'hello binding: 3000 https://google.com/'
    );

    await container.interceptOutboundHttps(
      '*',
      this.ctx.exports.TestService({ props: { id: 4000 } })
    );

    await this.expectHttpsIntercept(
      'example.com',
      200,
      'hello binding: 1000 https://example.com/'
    );

    await this.expectHttpsIntercept(
      'github.com',
      200,
      'hello binding: 4000 https://github.com/'
    );
  }

  async testSetEgressTcp() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    // Register a TCP egress mapping before the container starts.
    // When the container connects to 11.0.0.1:7777 via raw TCP, the
    // sidecar forwards the stream to our TestService.connect() handler.
    await container.interceptOutboundTcp(
      '11.0.0.1:7777',
      this.ctx.exports.TestService({ props: { id: 500 } })
    );

    container.start();

    container.monitor().catch((err) => {
      console.error('Container exited with an error:', err.message);
    });

    await this.waitUntilContainerIsHealthy();

    // Also register another mapping after the container is running.
    await container.interceptOutboundTcp(
      '11.0.0.2:7777',
      this.ctx.exports.TestService({ props: { id: 600 } })
    );

    // Ask the container to open a raw TCP connection to 11.0.0.1:7777.
    // The container's /intercept-tcp endpoint sends "ping\n" over the
    // socket. The sidecar intercepts it and routes it to
    // TestService.connect(), which reads the data and echoes it back
    // prefixed with the binding id.
    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept-tcp', {
          headers: { 'x-tcp-target': '11.0.0.1:7777' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      const body = await response.text();
      assert.equal(body, 'tcp binding: 500 got: ping');
    }

    // Verify the second mapping works too.
    {
      const response = await container
        .getTcpPort(8080)
        .fetch('http://foo/intercept-tcp', {
          headers: { 'x-tcp-target': '11.0.0.2:7777' },
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(response.status, 200);
      const body = await response.text();
      assert.equal(body, 'tcp binding: 600 got: ping');
    }
  }

  async testInterceptWebSocket() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);
    // Set up egress mapping to route WebSocket requests to the binding
    await container.interceptOutboundHttp(
      '11.0.0.1:9999',
      this.ctx.exports.TestService({ props: { id: 42 } })
    );

    // Start container with WebSocket proxy mode enabled
    container.start({
      env: { WS_ENABLED: 'true', WS_PROXY_TARGET: '11.0.0.1:9999' },
    });

    container.monitor().catch((_err) => {});

    // Wait for container to be available
    await this.waitUntilContainerIsHealthy();

    assert.strictEqual(container.running, true);

    // Connect to container's /ws endpoint which proxies to the intercepted address
    // Flow: DO -> container:8080/ws -> container connects to 11.0.0.1:9999/ws
    //       -> sidecar intercepts -> workerd -> TestService worker binding
    const res = await container.getTcpPort(8080).fetch('http://foo/ws', {
      headers: {
        Upgrade: 'websocket',
        Connection: 'Upgrade',
        'Sec-WebSocket-Key': 'x3JJHMbDL1EzLkh9GBhXDw==',
        'Sec-WebSocket-Version': '13',
      },
      signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
    });

    // Should get WebSocket upgrade response
    assert.strictEqual(res.status, 101);
    assert.strictEqual(res.headers.get('upgrade'), 'websocket');
    assert.strictEqual(!!res.webSocket, true);

    const ws = res.webSocket;
    ws.binaryType = 'arraybuffer';
    ws.accept();

    // Listen for response
    const { promise, resolve, reject } = Promise.withResolvers();

    ws.addEventListener(
      'message',
      (event) => {
        resolve(event.data);
      },
      { once: true }
    );

    const timeout = setTimeout(() => {
      reject(new Error('Websocket message not received within 5 seconds'));
    }, 5_000);

    // Send a test message - should go through the whole chain and come back
    ws.send('Hello through intercept!');

    // Should receive response from TestService binding with id 42
    const response = new TextDecoder().decode(await promise);
    clearTimeout(timeout);
    assert.strictEqual(response, 'Binding 42: Hello through intercept!');

    ws.close();
    await container.destroy();
  }

  async testInterceptWebSocketHttps() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    await container.interceptOutboundHttps(
      'example.com:443',
      this.ctx.exports.TestService({ props: { id: 99 } })
    );

    container.start({
      env: {
        WS_ENABLED: 'true',
        WS_PROXY_TARGET: 'example.com',
        WS_PROXY_SECURE: 'true',
        NODE_EXTRA_CA_CERTS:
          '/etc/cloudflare/certs/cloudflare-containers-ca.crt',
      },
    });

    container.monitor().finally(() => {
      console.log('Container exited');
    });

    await this.waitUntilContainerIsHealthy();

    assert.strictEqual(container.running, true);

    const res = await container.getTcpPort(8080).fetch('http://foo/ws', {
      headers: {
        Upgrade: 'websocket',
        Connection: 'Upgrade',
        'Sec-WebSocket-Key': 'x3JJHMbDL1EzLkh9GBhXDw==',
        'Sec-WebSocket-Version': '13',
      },
      signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
    });

    assert.strictEqual(res.status, 101);
    assert.strictEqual(res.headers.get('upgrade'), 'websocket');
    assert.strictEqual(!!res.webSocket, true);

    const ws = res.webSocket;
    ws.binaryType = 'arraybuffer';
    ws.accept();

    const { promise, resolve, reject } = Promise.withResolvers();

    ws.addEventListener(
      'message',
      (event) => {
        resolve(event.data);
      },
      { once: true }
    );

    const timeout = setTimeout(() => {
      reject(new Error('Websocket message not received within 5 seconds'));
    }, 5_000);

    ws.send('Hello through WSS intercept!');

    const response = new TextDecoder().decode(await promise);
    clearTimeout(timeout);
    assert.strictEqual(response, 'Binding 99: Hello through WSS intercept!');

    ws.close();
    await container.destroy();
  }

  async testSnapshotRoundTrip() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const writeResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/test.txt', {
        method: 'POST',
        body: 'snapshot-content-123',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(writeResp.status, 200);

    const snapshot = await container.snapshotDirectory({ dir: '/app/data' });
    assert.strictEqual(typeof snapshot.id, 'string');
    assert.ok(snapshot.id.length > 0, 'snapshot id should be non-empty');
    assert.ok(snapshot.size > 0, 'snapshot size should be > 0');
    assert.strictEqual(snapshot.dir, '/app/data');
    assert.strictEqual(snapshot.name, undefined);

    await container.destroy();
    await monitor;
    assert.strictEqual(container.running, false);

    container.start({
      enableInternet: true,
      directorySnapshots: [{ snapshot }],
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const readResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/test.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(readResp.status, 200);
    assert.strictEqual(await readResp.text(), 'snapshot-content-123');

    await container.destroy();
    await monitor2;
  }

  async testSnapshotNamedRoundTrip() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/named.txt', {
        method: 'POST',
        body: 'named-snapshot',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    const snapshot = await container.snapshotDirectory({
      dir: '/app/data',
      name: 'my-snapshot',
    });
    assert.strictEqual(snapshot.name, 'my-snapshot');
    assert.strictEqual(snapshot.dir, '/app/data');

    await container.destroy();
    await monitor;

    container.start({
      enableInternet: true,
      directorySnapshots: [{ snapshot }],
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const readResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/named.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(readResp.status, 200);
    assert.strictEqual(await readResp.text(), 'named-snapshot');

    await container.destroy();
    await monitor2;
  }

  async testSnapshotMultipleDirectories() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/dir1/file1.txt', {
        method: 'POST',
        body: 'content-dir1',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/dir2/file2.txt', {
        method: 'POST',
        body: 'content-dir2',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    const snap1 = await container.snapshotDirectory({ dir: '/app/dir1' });
    const snap2 = await container.snapshotDirectory({ dir: '/app/dir2' });

    await container.destroy();
    await monitor;

    container.start({
      enableInternet: true,
      directorySnapshots: [{ snapshot: snap1 }, { snapshot: snap2 }],
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const r1 = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/dir1/file1.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(r1.status, 200);
    assert.strictEqual(await r1.text(), 'content-dir1');

    const r2 = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/dir2/file2.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(r2.status, 200);
    assert.strictEqual(await r2.text(), 'content-dir2');

    await container.destroy();
    await monitor2;
  }

  async testSnapshotCustomMountPoint() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const writeResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/test.txt', {
        method: 'POST',
        body: 'remapped-content',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(writeResp.status, 200);

    const snapshot = await container.snapshotDirectory({ dir: '/app/data' });
    assert.strictEqual(snapshot.dir, '/app/data');

    await container.destroy();
    await monitor;

    container.start({
      enableInternet: true,
      directorySnapshots: [{ snapshot, mountPoint: '/app/restored' }],
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const readResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/restored/test.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(readResp.status, 200);
    assert.strictEqual(await readResp.text(), 'remapped-content');

    // Must not exist at original path since we restored to a different mount point
    const origResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/test.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(origResp.status, 404);

    await container.destroy();
    await monitor2;
  }

  async testSnapshotOverlappingMounts() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/tmp/parent-src/root.txt', {
        method: 'POST',
        body: 'parent-root',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    await container
      .getTcpPort(8080)
      .fetch(
        'http://foo/write-file?path=/tmp/parent-src/child/from-parent.txt',
        {
          method: 'POST',
          body: 'masked-by-child-mount',
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        }
      );
    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/tmp/child-src/from-child.txt', {
        method: 'POST',
        body: 'child-wins',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    const parentSnapshot = await container.snapshotDirectory({
      dir: '/tmp/parent-src',
    });
    const childSnapshot = await container.snapshotDirectory({
      dir: '/tmp/child-src',
    });

    await container.destroy();
    await monitor;

    const assertRestoredTree = async () => {
      const rootResp = await container
        .getTcpPort(8080)
        .fetch('http://foo/read-file?path=/tmp/restored/root.txt', {
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(rootResp.status, 200);
      assert.strictEqual(await rootResp.text(), 'parent-root');

      const childResp = await container
        .getTcpPort(8080)
        .fetch('http://foo/read-file?path=/tmp/restored/child/from-child.txt', {
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        });
      assert.equal(childResp.status, 200);
      assert.strictEqual(await childResp.text(), 'child-wins');

      const maskedResp = await container
        .getTcpPort(8080)
        .fetch(
          'http://foo/read-file?path=/tmp/restored/child/from-parent.txt',
          {
            signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
          }
        );
      assert.equal(maskedResp.status, 404);
    };

    const startAndAssert = async (directorySnapshots) => {
      container.start({ enableInternet: true, directorySnapshots });
      const restoreMonitor = container.monitor().catch((_err) => {});
      await this.waitUntilContainerIsHealthy();
      await assertRestoredTree();
      await container.destroy();
      await restoreMonitor;
    };

    await startAndAssert([
      { snapshot: childSnapshot, mountPoint: '/tmp/restored/child' },
      { snapshot: parentSnapshot, mountPoint: '/tmp/restored' },
    ]);

    await startAndAssert([
      { snapshot: parentSnapshot, mountPoint: '/tmp/restored' },
      { snapshot: childSnapshot, mountPoint: '/tmp/restored/child' },
    ]);
  }

  async testSnapshotDuplicateRestoreDirsRejected() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/tmp/dup-a/file-a.txt', {
        method: 'POST',
        body: 'dup-a',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/tmp/dup-b/file-b.txt', {
        method: 'POST',
        body: 'dup-b',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    const firstSnapshot = await container.snapshotDirectory({
      dir: '/tmp/dup-a',
    });
    const secondSnapshot = await container.snapshotDirectory({
      dir: '/tmp/dup-b',
    });

    await container.destroy();
    await monitor;

    await assert.rejects(
      () =>
        new Promise((resolve, reject) => {
          try {
            container.start({
              enableInternet: true,
              directorySnapshots: [
                { snapshot: firstSnapshot, mountPoint: '/tmp/duplicate' },
                { snapshot: secondSnapshot, mountPoint: '/tmp/duplicate/' },
              ],
            });
          } catch (err) {
            return reject(err);
          }
          container.monitor().then(resolve).catch(reject);
        }),
      (err) => {
        assert.strictEqual(err.message, 'Container failed to start');
        return true;
      }
    );

    assert.strictEqual(container.running, false);
  }

  async testSnapshotRestoreToRoot() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    const fakeSnapshot = {
      id: '01234567-89ab-cdef-0123-456789abcdef',
      size: 1024,
      dir: '/app/data',
    };

    assert.throws(
      () =>
        container.start({
          enableInternet: true,
          directorySnapshots: [{ snapshot: fakeSnapshot, mountPoint: '/' }],
        }),
      { message: /Directory snapshot cannot be restored to root directory\./ }
    );

    assert.strictEqual(container.running, false);
  }

  async testSnapshotRestoreImplicitRootRejected() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    const fakeSnapshot = {
      id: '11111111-2222-3333-4444-555555555555',
      size: 1024,
      dir: '/',
    };

    assert.throws(
      () =>
        container.start({
          enableInternet: true,
          directorySnapshots: [{ snapshot: fakeSnapshot }],
        }),
      { message: /Directory snapshot cannot be restored to root directory\./ }
    );

    assert.strictEqual(container.running, false);
  }

  async testSnapshotRestoreRelativeMountPointRejected() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    const fakeSnapshot = {
      id: 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee',
      size: 1024,
      dir: '/app/data',
    };

    assert.throws(
      () =>
        container.start({
          enableInternet: true,
          directorySnapshots: [
            { snapshot: fakeSnapshot, mountPoint: 'tmp/restored' },
          ],
        }),
      {
        message:
          /Directory snapshot restore path must be absolute\. Got: tmp\/restored/,
      }
    );

    assert.strictEqual(container.running, false);
  }

  async testSnapshotStoppedContainer() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    await assert.rejects(
      async () => container.snapshotDirectory({ dir: '/app/data' }),
      (err) => {
        assert.match(err.message, /not running/);
        return true;
      }
    );
  }

  async testSnapshotRestoreNonExistentId() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    const fakeSnapshot = {
      id: 'deadbeef-0000-0000-0000-000000000000',
      size: 1024,
      dir: '/app/data',
    };

    await assert.rejects(
      () =>
        new Promise((resolve, reject) => {
          try {
            container.start({
              enableInternet: true,
              directorySnapshots: [{ snapshot: fakeSnapshot }],
            });
          } catch (err) {
            return reject(err);
          }
          container.monitor().then(resolve).catch(reject);
        }),
      (err) => {
        assert.ok(err.message.length > 0, 'error should have a message');
        return true;
      }
    );
  }

  async testSnapshotNonExistentDirectory() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await assert.rejects(
      () => container.snapshotDirectory({ dir: '/does/not/exist' }),
      (err) => {
        assert.match(err.message, /directory not found/);
        return true;
      }
    );

    await container.destroy();
    await monitor;
  }

  async testContainerSnapshotRoundTrip() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    assert.strictEqual(container.running, false);

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const writeResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/full-snapshot.txt', {
        method: 'POST',
        body: 'full-snapshot-content',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(writeResp.status, 200);

    const tmpWriteResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/tmp/full-snapshot-tmp.txt', {
        method: 'POST',
        body: 'tmp-full-snapshot-content',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(tmpWriteResp.status, 200);

    const snapshot = await container.snapshotContainer({});
    assert.strictEqual(typeof snapshot.id, 'string');
    assert.ok(snapshot.id.length > 0, 'snapshot id should be non-empty');
    assert.ok(snapshot.size > 0, 'snapshot size should be > 0');
    assert.strictEqual(snapshot.name, undefined);

    await container.destroy();
    await monitor;
    assert.strictEqual(container.running, false);

    container.start({
      enableInternet: true,
      containerSnapshot: snapshot,
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const readResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/full-snapshot.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(readResp.status, 200);
    assert.strictEqual(await readResp.text(), 'full-snapshot-content');

    const tmpReadResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/tmp/full-snapshot-tmp.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(tmpReadResp.status, 200);
    assert.strictEqual(await tmpReadResp.text(), 'tmp-full-snapshot-content');

    await container.destroy();
    await monitor2;
  }

  async testContainerSnapshotNamedRoundTrip() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/full-named.txt', {
        method: 'POST',
        body: 'named-container-snapshot',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    const snapshot = await container.snapshotContainer({
      name: 'named-container-snapshot',
    });
    assert.strictEqual(snapshot.name, 'named-container-snapshot');

    await container.destroy();
    await monitor;

    container.start({
      enableInternet: true,
      containerSnapshot: snapshot,
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const readResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/full-named.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(readResp.status, 200);
    assert.strictEqual(await readResp.text(), 'named-container-snapshot');

    await container.destroy();
    await monitor2;
  }

  async testContainerSnapshotRestoreNonExistentId() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    const fakeSnapshot = {
      id: 'feedface-0000-0000-0000-000000000000',
      size: 1024,
    };

    await assert.rejects(
      () =>
        new Promise((resolve, reject) => {
          try {
            container.start({
              enableInternet: true,
              containerSnapshot: fakeSnapshot,
            });
          } catch (err) {
            return reject(err);
          }
          container.monitor().then(resolve).catch(reject);
        }),
      (err) => {
        assert.strictEqual(err.message, 'Container failed to start');
        return true;
      }
    );

    assert.strictEqual(container.running, false);
  }

  async testContainerSnapshotWithDirectoryOverlay() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/from-full.txt', {
        method: 'POST',
        body: 'from-full-snapshot',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/overlay-target/shared.txt', {
        method: 'POST',
        body: 'from-full-layer',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    await container
      .getTcpPort(8080)
      .fetch(
        'http://foo/write-file?path=/app/overlay-target/full-only-hidden.txt',
        {
          method: 'POST',
          body: 'hidden-by-overlay',
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        }
      );
    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/tmp/overlay-source/shared.txt', {
        method: 'POST',
        body: 'from-directory-overlay',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    await container
      .getTcpPort(8080)
      .fetch(
        'http://foo/write-file?path=/tmp/overlay-source/overlay-only.txt',
        {
          method: 'POST',
          body: 'overlay-only-content',
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        }
      );

    const directorySnapshot = await container.snapshotDirectory({
      dir: '/tmp/overlay-source',
    });
    const containerSnapshot = await container.snapshotContainer({
      name: 'container-with-overlay',
    });

    await container.destroy();
    await monitor;

    container.start({
      enableInternet: true,
      containerSnapshot,
      directorySnapshots: [
        { snapshot: directorySnapshot, mountPoint: '/app/overlay-target' },
      ],
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const fullResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/from-full.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(fullResp.status, 200);
    assert.strictEqual(await fullResp.text(), 'from-full-snapshot');

    const overlayResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/overlay-target/shared.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(overlayResp.status, 200);
    assert.strictEqual(await overlayResp.text(), 'from-directory-overlay');

    const overlayOnlyResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/overlay-target/overlay-only.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(overlayOnlyResp.status, 200);
    assert.strictEqual(await overlayOnlyResp.text(), 'overlay-only-content');

    const hiddenFullResp = await container
      .getTcpPort(8080)
      .fetch(
        'http://foo/read-file?path=/app/overlay-target/full-only-hidden.txt',
        {
          signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
        }
      );
    assert.equal(hiddenFullResp.status, 404);

    await container.destroy();
    await monitor2;
  }

  async testContainerSnapshotExcludesDirectoryMounts() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/tmp/mounted-source/mounted.txt', {
        method: 'POST',
        body: 'from-mounted-directory',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    const directorySnapshot = await container.snapshotDirectory({
      dir: '/tmp/mounted-source',
    });

    await container.destroy();
    await monitor;

    container.start({
      enableInternet: true,
      directorySnapshots: [
        { snapshot: directorySnapshot, mountPoint: '/app/mounted' },
      ],
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const mountedResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/mounted/mounted.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(mountedResp.status, 200);
    assert.strictEqual(await mountedResp.text(), 'from-mounted-directory');

    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/local-after-mount.txt', {
        method: 'POST',
        body: 'container-local-state',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    const containerSnapshot = await container.snapshotContainer({
      name: 'exclude-mounted-directory',
    });

    await container.destroy();
    await monitor2;

    container.start({
      enableInternet: true,
      containerSnapshot,
    });
    const monitor3 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const localResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/local-after-mount.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(localResp.status, 200);
    assert.strictEqual(await localResp.text(), 'container-local-state');

    const missingMountedResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/mounted/mounted.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(missingMountedResp.status, 404);

    await container.destroy();
    await monitor3;
  }

  async testContainerSnapshotRelayerWithDirectoryMounts() {
    const container = this.ctx.container;
    if (container.running) {
      const monitor = container.monitor().catch((_err) => {});
      await container.destroy();
      await monitor;
    }

    container.start({ enableInternet: true });
    const monitor = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/tmp/relayer-source/overlay.txt', {
        method: 'POST',
        body: 'overlay-after-relayer',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    const directorySnapshot = await container.snapshotDirectory({
      dir: '/tmp/relayer-source',
    });

    await container.destroy();
    await monitor;

    container.start({
      enableInternet: true,
      directorySnapshots: [
        { snapshot: directorySnapshot, mountPoint: '/app/relayer' },
      ],
    });
    const monitor2 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    await container
      .getTcpPort(8080)
      .fetch('http://foo/write-file?path=/app/data/re-layered.txt', {
        method: 'POST',
        body: 'from-full-container-snapshot',
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });

    const containerSnapshot = await container.snapshotContainer({
      name: 'relayer-container-snapshot',
    });

    await container.destroy();
    await monitor2;

    container.start({
      enableInternet: true,
      containerSnapshot,
      directorySnapshots: [
        { snapshot: directorySnapshot, mountPoint: '/app/relayer' },
      ],
    });
    const monitor3 = container.monitor().catch((_err) => {});
    await this.waitUntilContainerIsHealthy();

    const relayerResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/relayer/overlay.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(relayerResp.status, 200);
    assert.strictEqual(await relayerResp.text(), 'overlay-after-relayer');

    const fullResp = await container
      .getTcpPort(8080)
      .fetch('http://foo/read-file?path=/app/data/re-layered.txt', {
        signal: AbortSignal.timeout(DEFAULT_TIMEOUT_DURATION),
      });
    assert.equal(fullResp.status, 200);
    assert.strictEqual(await fullResp.text(), 'from-full-container-snapshot');

    await container.destroy();
    await monitor3;
  }
}

export class TestService extends WorkerEntrypoint {
  fetch(request) {
    // Check if this is a WebSocket upgrade request
    const upgradeHeader = request.headers.get('Upgrade');
    if (upgradeHeader && upgradeHeader.toLowerCase() === 'websocket') {
      // Handle WebSocket upgrade
      const [client, server] = Object.values(new WebSocketPair());

      server.binaryType = 'arraybuffer';
      server.accept();

      server.addEventListener('message', (event) => {
        // Echo back with binding id prefix
        server.send(
          `Binding ${this.ctx.props.id}: ${new TextDecoder().decode(event.data)}`
        );
      });

      return new Response(null, {
        status: 101,
        webSocket: client,
      });
    }

    // Regular HTTP request
    return new Response(
      'hello binding: ' + this.ctx.props.id + ' ' + request.url
    );
  }

  // Handle raw TCP connections forwarded by interceptOutboundTcp.
  // The socket has .readable / .writable streams. We read until we
  // see a newline delimiter, then write back a response that includes
  // the binding id and the received message.
  async connect(socket) {
    const id = this.ctx.props.id;
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    // Read from the socket until we hit a newline.
    const reader = socket.readable.getReader();
    let incoming = '';
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      incoming += dec.decode(value, { stream: true });
      if (incoming.includes('\n')) break;
    }
    incoming += dec.decode();
    reader.releaseLock();

    // Write response and close.
    const writer = socket.writable.getWriter();
    await writer.write(
      enc.encode(`tcp binding: ${id} got: ${incoming.trim()}`)
    );
    await writer.close();
  }
}

export class DurableObjectExample2 extends DurableObjectExample {}

// Test basic container status
export const testStatus = {
  async test(_ctrl, env) {
    for (const CONTAINER of [env.MY_CONTAINER, env.MY_DUPLICATE_CONTAINER]) {
      for (const name of ['testStatus', 'testStatus2']) {
        const id = CONTAINER.idFromName(getRandomDurableObjectName(name));
        const stub = CONTAINER.get(id);
        assert.strictEqual(await stub.getStatus(), false);
      }
    }
  },
};

// Test basic container functionality
export const testBasics = {
  async test(_ctrl, env) {
    for (const CONTAINER of [env.MY_CONTAINER, env.MY_DUPLICATE_CONTAINER]) {
      const id = CONTAINER.idFromName(getRandomDurableObjectName('testBasics'));
      const stub = CONTAINER.get(id);
      await stub.testBasics();
    }
  },
};

// Test a variety of common exec() workflows.
export const testExec = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testExec')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testExec();
  },
};

// Test exit code monitor functionality
export const testExitCode = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testExitCode')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testExitCode();
  },
};

// Test WebSocket functionality
export const testWebSockets = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testWebsockets')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testWs();
  },
};

export const testPortNotListening = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testPortNotListening')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testPortNotListening();
  },
};

// Test alarm functionality with containers
export const testAlarm = {
  async test(_ctrl, env) {
    // Test that we can recover the use_containers flag correctly in setAlarm
    // after a DO has been evicted
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testAlarm')
    );
    let stub = env.MY_CONTAINER.get(id);

    // Start immediate alarm
    await stub.startAlarm(true, 0);

    // Wait for alarm to trigger
    let retries = 0;
    while ((await stub.getAlarmIndex()) === 0 && retries < 50) {
      await scheduler.wait(20);
      retries++;
    }

    await scheduler.wait(50);

    // Set alarm for future and abort
    await stub.startAlarm(false, 1000);

    try {
      await stub.abort();
    } catch {
      // Expected to throw
    }

    stub = env.MY_CONTAINER.get(id);
    let confirmed = false;
    for (let i = 0; i < 50 && !confirmed; i++) {
      try {
        await stub.checkAlarmAbortConfirmation();
        confirmed = true;
      } catch (e) {
        assert.match(
          e.message,
          /Abort confirmation did not get inserted/,
          `Unexpected error while polling for alarm: ${e.message}`
        );

        await scheduler.wait(100);
      }
    }
    if (!confirmed) {
      await stub.checkAlarmAbortConfirmation();
    }
  },
};

export const testContainerShutdown = {
  async test(_, env) {
    const name = getRandomDurableObjectName('testContainerShutdown');

    {
      const stub = env.MY_CONTAINER.getByName(name);
      await stub.start();
      await assert.rejects(() => stub.abort(), {
        name: 'Error',
        message: 'Application called abort() to reset Durable Object.',
      });
    }

    // Wait for the container to be shutdown after the DO aborts
    await scheduler.wait(500);

    {
      const stub = env.MY_CONTAINER.getByName(name);

      // Container should not be running after DO exited
      await stub.expectRunning(false);
    }
  },
};

export const testSetInactivityTimeout = {
  async test(_ctrl, env) {
    const name = getRandomDurableObjectName('testSetInactivityTimeout');

    {
      const stub = env.MY_CONTAINER.getByName(name);

      await stub.testSetInactivityTimeout(10_000);

      await assert.rejects(() => stub.abort(), {
        name: 'Error',
        message: 'Application called abort() to reset Durable Object.',
      });
    }

    // Here we wait to ensure that if setInactivityTimeout *doesn't* work, the
    // container has enough time to shutdown after the DO is aborted. If we
    // don't wait then ctx.container.running will always be true, even without
    // setInactivityTimeout, because the container won't have stoped yet.
    await scheduler.wait(500);

    {
      const stub = env.MY_CONTAINER.getByName(name);

      // Container should still be running after DO exited
      await stub.expectRunning(true);
    }
  },
};

// Test that custom labels are passed through to the container
export const testLabels = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testLabels')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testLabels();
  },
};

// Test that invalid labels are rejected with clear error messages
export const testLabelValidation = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testLabelValidation')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testLabelValidation();
  },
};

export const testInspectBeforeStart = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testInspectBeforeStart')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testInspectBeforeStart();
  },
};

export const testInspectEmptyLabels = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testInspectEmptyLabels')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testInspectEmptyLabels();
  },
};

export const testInspectAfterDestroy = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testInspectAfterDestroy')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testInspectAfterDestroy();
  },
};

// Test PID namespace isolation behavior
// When containers_pid_namespace is ENABLED, the container has its own isolated PID namespace.
// We verify this by checking that PID 1 in the container's namespace is the container's
// init process, not the host's init process (systemd, launchd, etc.).
export const testPidNamespace = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testPidNamespace')
    );
    const stub = env.MY_CONTAINER.get(id);
    const data = await stub.testPidNamespace();

    // When using an isolated PID namespace, PID 1 should be the container's entrypoint
    // (a bash script that runs node), not the host's init process (systemd, launchd, init).
    assert.match(
      data.init,
      /container-client-test/,
      `Expected PID 1 to be the container entrypoint, but got: ${data.init}`
    );
  },
};

// Test setEgressHttp hostname functionality with internet (check we can establish
// outbound with others).
export const testSetEgressHttpWithInternet = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSetEgressHttpWithInternet')
    );
    let stub = env.MY_CONTAINER.get(id);
    await stub.testSetEgressHttpWithInternet();
  },
};

// Test setEgressHttp hostname functionality with no internet
export const testSetEgressHttpNoInternet = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSetEgressHttpNoInternet')
    );
    let stub = env.MY_CONTAINER.get(id);
    await stub.testSetEgressHttpNoInternet();
  },
};

// Test setEgressHttp functionality - registers a binding's channel token with the container
export const testSetEgressHttp = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSetEgressHttp')
    );
    let stub = env.MY_CONTAINER.get(id);
    await stub.testSetEgressHttp();
    try {
      // test we recover from aborts
      await stub.abort();
    } catch {
      // intentionally empty
    }

    stub = env.MY_CONTAINER.get(id);
    // should work idempotent
    await stub.testSetEgressHttp();
  },
};

export const testSetEgressHttps = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSetEgressHttps')
    );
    let stub = env.MY_CONTAINER.get(id);
    await stub.testSetEgressHttps();
    try {
      await stub.abort();
    } catch {
      // intentionally empty — abort may throw if container already stopped
    }

    stub = env.MY_CONTAINER.get(id);
    await stub.testSetEgressHttps();
  },
};

export const testSetEgressTcp = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSetEgressTcp')
    );
    let stub = env.MY_CONTAINER.get(id);
    await stub.testSetEgressTcp();
    try {
      await stub.abort();
    } catch {
      // intentionally empty
    }

    stub = env.MY_CONTAINER.get(id);
    await stub.testSetEgressTcp();
  },
};

// Test WebSocket through interceptOutboundHttp - DO -> container -> worker binding via WebSocket
export const testInterceptWebSocket = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testInterceptWebSocket')
    );

    const stub = env.MY_CONTAINER.get(id);
    await stub.testInterceptWebSocket();
  },
};

export const testInterceptWebSocketHttps = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testInterceptWebSocketHttps')
    );

    const stub = env.MY_CONTAINER.get(id);
    await stub.testInterceptWebSocketHttps();
  },
};

// Test snapshot round-trip: write file -> snapshot -> destroy -> restore -> verify
export const testSnapshotRoundTrip = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotRoundTrip')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotRoundTrip();
  },
};

// Test snapshot with a human-friendly name
export const testSnapshotNamedRoundTrip = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotNamedRoundTrip')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotNamedRoundTrip();
  },
};

// Test snapshotting multiple directories and restoring them all
export const testSnapshotMultipleDirectories = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotMultipleDirectories')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotMultipleDirectories();
  },
};

// Test that snapshotting a non-existent directory gives a clear error
export const testSnapshotNonExistentDirectory = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotNonExistentDirectory')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotNonExistentDirectory();
  },
};

// Test restoring a snapshot to a different path than where it was captured
export const testSnapshotCustomMountPoint = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotCustomMountPoint')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotCustomMountPoint();
  },
};

// Test that start() rejects an explicit root restore mount point.
export const testSnapshotRestoreToRoot = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotRestoreToRoot')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotRestoreToRoot();
  },
};

// Test that overlapping restore paths work regardless of the user-supplied mount order.
export const testSnapshotOverlappingMounts = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotOverlappingMounts')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotOverlappingMounts();
  },
};

// Test that duplicate effective restore paths are rejected after normalization.
export const testSnapshotDuplicateRestoreDirsRejected = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotDuplicateRestoreDirsRejected')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotDuplicateRestoreDirsRejected();
  },
};

// Test that start() also rejects implicit root restore via snapshot.dir.
export const testSnapshotRestoreImplicitRootRejected = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotRestoreImplicitRootRejected')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotRestoreImplicitRootRejected();
  },
};

// Test that start() rejects relative restore mount points at the API boundary.
export const testSnapshotRestoreRelativeMountPointRejected = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName(
        'testSnapshotRestoreRelativeMountPointRejected'
      )
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotRestoreRelativeMountPointRejected();
  },
};

// Test that snapshotDirectory() on a stopped container gives a clear error
export const testSnapshotStoppedContainer = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotStoppedContainer')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotStoppedContainer();
  },
};

// Test that restoring a snapshot with a nonexistent ID fails
export const testSnapshotRestoreNonExistentId = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotRestoreNonExistentId')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testSnapshotRestoreNonExistentId();
  },
};

// Test that a snapshot created by one DO can be sent to another DO and restored there.
export const testSnapshotCrossDoRestore = {
  async test(_ctrl, env) {
    const sourceId = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotCrossDoRestore-source')
    );
    const targetId = env.MY_DUPLICATE_CONTAINER.idFromName(
      getRandomDurableObjectName('testSnapshotCrossDoRestore-target')
    );

    const source = env.MY_CONTAINER.get(sourceId);
    const target = env.MY_DUPLICATE_CONTAINER.get(targetId);

    const snapshot = await source.createSnapshotForTransfer();
    assert.strictEqual(snapshot.dir, '/app/data');
    assert.strictEqual(snapshot.name, 'cross-do-snapshot');

    await target.restoreTransferredSnapshot(snapshot);
  },
};

// Test full container snapshot round-trip: write file -> snapshot -> destroy -> restore -> verify
export const testContainerSnapshotRoundTrip = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testContainerSnapshotRoundTrip')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testContainerSnapshotRoundTrip();
  },
};

// Test full container snapshot with a human-friendly name.
export const testContainerSnapshotNamedRoundTrip = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testContainerSnapshotNamedRoundTrip')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testContainerSnapshotNamedRoundTrip();
  },
};

// Test that a full container snapshot created by one DO can be restored by another.
export const testContainerSnapshotCrossDoRestore = {
  async test(_ctrl, env) {
    const sourceId = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testContainerSnapshotCrossDoRestore-source')
    );
    const targetId = env.MY_DUPLICATE_CONTAINER.idFromName(
      getRandomDurableObjectName('testContainerSnapshotCrossDoRestore-target')
    );

    const source = env.MY_CONTAINER.get(sourceId);
    const target = env.MY_DUPLICATE_CONTAINER.get(targetId);

    const snapshot = await source.createContainerSnapshotForTransfer();
    assert.strictEqual(snapshot.name, 'cross-do-container-snapshot');

    await target.restoreTransferredContainerSnapshot(snapshot);
  },
};

// Test that restoring a full container snapshot with a nonexistent ID fails.
export const testContainerSnapshotRestoreNonExistentId = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testContainerSnapshotRestoreNonExistentId')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testContainerSnapshotRestoreNonExistentId();
  },
};

// Test that a full container snapshot can be layered with a directory snapshot restore.
export const testContainerSnapshotWithDirectoryOverlay = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testContainerSnapshotWithDirectoryOverlay')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testContainerSnapshotWithDirectoryOverlay();
  },
};

// Test that full container snapshots exclude active directory snapshot mounts.
export const testContainerSnapshotExcludesDirectoryMounts = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName('testContainerSnapshotExcludesDirectoryMounts')
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testContainerSnapshotExcludesDirectoryMounts();
  },
};

// Test that a full container snapshot taken while directory snapshots are active can be re-layered.
export const testContainerSnapshotRelayerWithDirectoryMounts = {
  async test(_ctrl, env) {
    const id = env.MY_CONTAINER.idFromName(
      getRandomDurableObjectName(
        'testContainerSnapshotRelayerWithDirectoryMounts'
      )
    );
    const stub = env.MY_CONTAINER.get(id);
    await stub.testContainerSnapshotRelayerWithDirectoryMounts();
  },
};
