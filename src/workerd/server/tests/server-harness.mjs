// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import childProcess from 'node:child_process';
import assert from 'node:assert';

// A convenience class to:
// - start workerd
// - wait for its listen ports to be opened and reported
// - stop workerd
export class WorkerdServerHarness {
  // Properties set by our constructor and never changed.
  #workerdBinary = null;
  #workerdConfig = null;
  #listenPortNames = null;
  #extraArgs = null;
  #startupTimeoutMs = null;

  // Properties set by `start()` and cleared by `stop()`.
  #child = null;
  #listenPorts = null;
  #listenInspectorPort = null;
  #closed = null;
  #pendingReadiness = null;
  #lastControlMessage = null;

  constructor({
    workerdBinary,
    workerdConfig,
    listenPortNames,
    extraArgs = [],
    startupTimeoutMs = 30000,
  }) {
    this.#workerdBinary = workerdBinary;
    this.#workerdConfig = workerdConfig;
    this.#listenPortNames = listenPortNames;
    this.#extraArgs = extraArgs;
    this.#startupTimeoutMs = startupTimeoutMs;
  }

  // Spawn our workerd process and wait for it to start.
  async start() {
    assert.equal(this.#child, null);

    // One after STDIN, STDOUT, and STDERR.
    const CONTROL_FD = 3;

    const args = [
      'serve',
      this.#workerdConfig,
      '--verbose',
      '--inspector-addr=127.0.0.1:0',
      `--control-fd=${CONTROL_FD}`,
      ...this.#extraArgs,
    ];

    const options = {
      stdio: [
        'inherit',
        'inherit',
        'inherit',
        // One more for our control FD.
        'pipe',
      ],
    };

    // Start the subprocess.
    console.log('[HARNESS] Starting workerd with args:', args);
    this.#child = childProcess.spawn(this.#workerdBinary, args, options);
    this.#pendingReadiness = new Map();
    this.#lastControlMessage = 'none';
    this.#listenPorts = new Map(
      this.#listenPortNames.map((name) => [
        name,
        this.#expectReadiness(`listen:${name}`),
      ])
    );
    this.#listenInspectorPort = this.#expectReadiness('listen-inspector');
    const spawned = this.#expectReadiness('spawn');

    this.#closed = new Promise((resolve) => {
      this.#child.once('close', (code, signal) => resolve([code, signal]));
    });
    this.#watchChild(this.#child.stdio[CONTROL_FD]);

    const timeout = setTimeout(() => {
      this.#failReadiness(
        `timed out after ${this.#startupTimeoutMs}ms waiting for ${[
          ...this.#pendingReadiness.keys(),
        ].join(', ')}`
      );
    }, this.#startupTimeoutMs);
    // Observe every readiness promise so an unused inspector promise cannot cause an unhandled
    // rejection. Callers receive readiness failures when they await their requested listen port.
    Promise.all([
      spawned,
      this.#listenInspectorPort,
      ...this.#listenPorts.values(),
    ]).then(
      () => clearTimeout(timeout),
      () => clearTimeout(timeout)
    );
    await spawned;
  }

  #expectReadiness(name) {
    const deferred = Promise.withResolvers();
    this.#pendingReadiness.set(name, deferred);
    return deferred.promise;
  }

  #resolveReadiness(name, value) {
    const deferred = this.#pendingReadiness.get(name);
    if (deferred !== undefined) {
      this.#pendingReadiness.delete(name);
      deferred.resolve(value);
    }
  }

  #failReadiness(reason) {
    if (!this.#pendingReadiness?.size) return;
    const error = new Error(
      `Workerd startup failed (PID ${this.#child.pid ?? 'unassigned'}): ${reason}; ` +
        `last control message: ${this.#lastControlMessage}`
    );
    for (const deferred of this.#pendingReadiness.values()) {
      deferred.reject(error);
    }
    this.#pendingReadiness.clear();
  }

  #watchChild(control) {
    this.#child.once('spawn', () => {
      console.log('[HARNESS] Workerd process spawned');
      this.#resolveReadiness('spawn');
    });
    this.#child.once('error', (error) => this.#failReadiness(error.message));
    this.#child.once('exit', (code, signal) => {
      this.#failReadiness(`process exited (code ${code}, signal ${signal})`);
    });
    control.once('end', () => {
      this.#failReadiness('control stream ended before readiness');
    });
    control.once('error', (error) => this.#failReadiness(error.message));
    control.setEncoding('utf8');
    let buffer = '';
    control.on('data', (data) => {
      buffer += data;
      let newline;
      while ((newline = buffer.indexOf('\n')) !== -1) {
        const line = buffer.slice(0, newline);
        buffer = buffer.slice(newline + 1);
        this.#readControlMessage(line);
      }
    });
  }

  #readControlMessage(line) {
    let message;
    try {
      message = JSON.parse(line);
    } catch (error) {
      this.#failReadiness(`invalid control message ${line}: ${error.message}`);
      return;
    }
    this.#lastControlMessage = line;
    console.log('[HARNESS] Control message:', message);
    const name =
      message.event === 'listen' ? `listen:${message.socket}` : message.event;
    this.#resolveReadiness(name, message.port);
  }

  // Return a promise for the inspector port.
  async getListenInspectorPort() {
    assert.notEqual(this.#listenInspectorPort, null);
    return await this.#listenInspectorPort;
  }

  // Return a promise for the named listen port.
  async getListenPort(name) {
    assert.notEqual(this.#listenPorts, null);
    assert.notEqual(this.#listenPorts.get(name), undefined);
    return await this.#listenPorts.get(name);
  }

  // Send SIGTERM to workerd and wait for it to completely finish.
  async stop() {
    assert.notEqual(this.#child, null);

    console.log('[HARNESS] Sending SIGTERM to workerd...');

    // First try SIGTERM with a timeout
    const SIGTERM_TIMEOUT = 5000; // 5 seconds
    const SIGKILL_TIMEOUT = 2000; // 2 more seconds for SIGKILL

    let result;
    let killed = false;

    // Set up a timeout for SIGTERM
    const sigtermTimeout = setTimeout(() => {
      if (!killed) {
        console.log('[HARNESS] SIGTERM timeout, sending SIGKILL...');
        this.#child.kill('SIGKILL');
        killed = true;
      }
    }, SIGTERM_TIMEOUT);

    // Set up a final timeout for SIGKILL
    const sigkillTimeout = setTimeout(() => {
      if (!killed) {
        console.error(
          '[HARNESS] Process did not respond to signals, forcing termination'
        );
        // This shouldn't happen, but just in case...
        process.exit(1);
      }
    }, SIGTERM_TIMEOUT + SIGKILL_TIMEOUT);

    try {
      // Send SIGTERM
      this.#child.kill('SIGTERM');

      // Wait for the process to close
      result = await this.#closed;
      killed = true;
      console.log('[HARNESS] Workerd process terminated successfully');
    } finally {
      // Clean up timeouts
      clearTimeout(sigtermTimeout);
      clearTimeout(sigkillTimeout);
    }

    this.#child = null;
    this.#listenPorts = null;
    this.#listenInspectorPort = null;
    this.#closed = null;
    this.#pendingReadiness = null;
    this.#lastControlMessage = null;

    return result;
  }
}
