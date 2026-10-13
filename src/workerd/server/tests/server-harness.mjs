// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { spawn } from 'node:child_process';
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

  // Properties set by `start()` and cleared by `stop()`.
  #child = null;
  #listenPorts = null;
  #listenInspectorPort = null;
  #closed = null;

  constructor({
    workerdBinary,
    workerdConfig,
    listenPortNames,
    extraArgs = [],
  }) {
    this.#workerdBinary = workerdBinary;
    this.#workerdConfig = workerdConfig;
    this.#listenPortNames = listenPortNames;
    this.#extraArgs = extraArgs;
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
    this.#child = spawn(this.#workerdBinary, args, options);

    // Create a promise for every named listen port we were told in our constructor to expect, and
    // resolve them as the control FD reports ports coming online. The control FD carries one JSON
    // object per line; a read may return several lines at once or a partial one, so the stream is
    // split on newlines before parsing.
    const listeners = new Set();
    let buffered = '';
    this.#child.stdio[CONTROL_FD].on('data', (data) => {
      buffered += data;
      const lines = buffered.split('\n');
      buffered = lines.pop();
      for (const line of lines) {
        if (line.length === 0) continue;
        const parsed = JSON.parse(line);
        console.log('[HARNESS] Control message:', parsed);
        for (const listener of listeners) listener(parsed);
      }
    });

    this.#listenPorts = new Map();
    for (const listenPort of this.#listenPortNames) {
      this.#listenPorts.set(
        listenPort,
        new Promise((resolve, reject) => {
          listeners.add((parsed) => {
            if (parsed.event === 'listen' && parsed.socket === listenPort) {
              resolve(parsed.port);
            }
          });
          this.#child.once('error', reject);
        })
      );
    }

    // Do the same as the above for the inspector port.
    this.#listenInspectorPort = new Promise((resolve, reject) => {
      listeners.add((parsed) => {
        if (parsed.event === 'listen-inspector') {
          resolve(parsed.port);
        }
      });
      this.#child.once('error', reject);
    });

    // Set up a closed promise, too.
    this.#closed = new Promise((resolve, reject) => {
      this.#child
        .once('close', (code, signal) => resolve([code, signal]))
        .once('error', reject);
    });

    // Wait for the subprocess to complete spawning before we return.
    await new Promise((resolve, reject) => {
      this.#child.once('spawn', resolve).once('error', reject);
      console.log('[HARNESS] Workerd process spawned');
    });
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

    return result;
  }
}
