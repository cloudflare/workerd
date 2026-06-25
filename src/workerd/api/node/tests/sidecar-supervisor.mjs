// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/*
 * Sidecars test framework
 * ------------------------
 *
 *  A `wd_test` may specify a `sidecar` to run alongside it. The sidecar is an auxiliary server
 *  process that runs alongside the test to provide realistic network endpoints.
 *
 * The sidecar and test processes are run together by the `sidecar_supervisor` (this file).
 * The supervisor is responsible for assigning a random IP address for the sidecar and test to
 * communicate (stored in the environment variable `SIDECAR_HOSTNAME`).
 *
 * Port assignment happens in one of two modes, selected via the `SIDECAR_PORT_MODE` environment
 * variable (set by the wd_test rule from its `sidecar_port_mode` attribute):
 *
 *  - "report" (default): the sidecar itself binds each socket to a kernel-assigned port
 *    (`listen(0)`) and reports the chosen port back to the supervisor over its stdout, one line
 *    per binding, in the form `<ENV_VAR_NAME>=<PORT>`. The supervisor reads those lines,
 *    populates the test's environment with the matching variables, and only then spawns the
 *    test process. Any other sidecar stdout is forwarded verbatim so it stays visible in test
 *    logs. This protocol is race-free and is preferred for sidecars under our control.
 *
 *  - "preallocate": the supervisor binds each port itself (port 0, kernel-assigned), records the
 *    chosen port, closes the socket, exports `<NAME>=<PORT>` in the environment, and only then
 *    spawns the sidecar. This exists for sidecars that can't participate in the report protocol
 *    (currently just the WPT entrypoint, which is a shell wrapper around upstream wpt.py and
 *    reads `$HTTP_PORT` etc. from the environment). Between the supervisor's `close()` and the
 *    sidecar's `bind()` the kernel can in principle hand out the port to an unrelated process;
 *    in practice the window is small and the race is rare. If it manifests as test flakiness,
 *    the right response is bazel's existing flaky-test handling — mark the affected test with
 *    `tags = ["flaky"]`.
 *
 * This architecture is designed to allow running unmodified TCP servers, such as wptserve for the
 * WPT tests. If necessary, IP address randomization can be disabled by setting
 * `sidecar_randomize_ip` to False.
 *
 *
 * ┌───────────────────────────────────────────────────────┐
 * │                                                       │
 * │                       bazel test                      │
 * │                                                       │
 * └───────────────────────────┬───────────────────────────┘
 *                             │
 *           env vars: PORTS_TO_ASSIGN, SIDECAR_PORT_MODE, SIDECAR_COMMAND
 *                             ▼
 * ┌───────────────────────────────────────────────────────┐
 * │                                                       │
 * │                   sidecar supervisor                  ├────────────────────────────────────────────────────────────┐
 * │                                                       │                              env vars: SIDECAR_HOSTNAME    │
 * └───────────────────────────┬───────────────────────────┘                       (+ NAME=PORT in preallocate mode)    │
 *                             │                          ▲                                                             ▼
 *                             │                       stdout: "<NAME>=<PORT>"         ┌──────────────────────────────────────────────┐
 *      env vars: SIDECAR_HOSTNAME, SERVER_PORT, ...      │     (report mode only)     │                                              │
 *                             ▼                          └────────────────────────────┤                   sidecar                    │
 * ┌───────────────────────────────────────────────────────┐                           │                                              │
 * │                                                       │                           └──────────────────────────────────────────────┘
 * │                        wd-test                        │                                                            ▲
 * │                                                       │                                                            │
 * └───────────────────────────┬───────────────────────────┘                                                            │
 *                             │                                                                                        │
 *    env var bindings: SIDECAR_HOSTNAME, SERVER_PORT, ...                                                              │
 *                             ▼                                                                                        │
 * ┌───────────────────────────────────────────────────────┐                                                            │
 * │                                                       │                                                            │
 * │                          test                         ├────────────tcp:─SIDECAR_HOSTNAME,─SERVER_PORT──────────────┘
 * │                                                       │
 * └───────────────────────────────────────────────────────┘
 */

import net from 'node:net';
import child_process from 'node:child_process';
import crypto from 'node:crypto';
import readline from 'node:readline';

// Matches `<NAME>=<PORT>` lines emitted by the sidecar to report bound ports.
const PORT_LINE = /^([A-Z_][A-Z0-9_]*)=(\d+)$/;

// Tunables for the preallocate path.
const ANY_PORT = 0;
const CONNECT_POLL_INTERVAL_MS = 500;

// === Report-mode helpers ====================================================

function spawnSidecarAndCaptureReportedPorts(cmd, expectedNames) {
  const proc = child_process.spawn(cmd, {
    shell: true,
    // Pipe stdout so we can read the sidecar's port reports. Stderr is inherited so log output
    // from the sidecar still surfaces in the test log without us having to forward it manually.
    stdio: ['inherit', 'pipe', 'inherit'],
  });

  const expected = new Set(expectedNames);
  const captured = {};
  const {
    promise: portsPromise,
    resolve: portsResolve,
    reject: portsReject,
  } = Promise.withResolvers();
  const { promise: exitPromise, resolve: exitResolve } =
    Promise.withResolvers();

  const rl = readline.createInterface({
    input: proc.stdout,
    crlfDelay: Infinity,
  });
  rl.on('line', (line) => {
    const m = line.match(PORT_LINE);
    if (m && expected.has(m[1]) && !(m[1] in captured)) {
      captured[m[1]] = m[2];
      if (Object.keys(captured).length === expected.size) {
        portsResolve({ ...captured });
      }
    } else {
      // Forward non-protocol lines so sidecar stdout stays visible in test logs.
      process.stdout.write(`${line}\n`);
    }
  });

  proc.once('exit', (code, signal) => {
    exitResolve(code);
    // If the sidecar dies before reporting all expected ports, surface that as a failure rather
    // than hanging indefinitely.
    if (Object.keys(captured).length < expected.size) {
      const missing = [...expected].filter((n) => !(n in captured));
      portsReject(
        new Error(
          `sidecar exited (code=${code}, signal=${signal}) before reporting ports: ${missing.join(', ')}`
        )
      );
    }
  });

  return { proc, exitPromise, portsPromise };
}

async function runReportMode({ portsToAssign, sidecarCommand, testArgv }) {
  const sidecar = spawnSidecarAndCaptureReportedPorts(
    sidecarCommand,
    portsToAssign
  );
  const ports = await sidecar.portsPromise;
  Object.assign(process.env, ports);

  process.exitCode = child_process.spawnSync(testArgv[0], testArgv.slice(1), {
    stdio: ['inherit', 'inherit', 'inherit'],
  }).status;

  sidecar.proc.kill();
  await sidecar.exitPromise;

  // Release the sidecar's piped stdout so Node doesn't keep the event loop alive waiting for it.
  sidecar.proc.stdout?.destroy();
}

// === Preallocate-mode helpers ===============================================

function getListeningServer(hostname) {
  const { promise, resolve } = Promise.withResolvers();
  const server = net.createServer();
  server.listen(ANY_PORT, hostname).once('listening', () => resolve(server));
  return promise;
}

function closeServer(server) {
  const { promise, resolve } = Promise.withResolvers();
  server.close(resolve);
  return promise;
}

// Bind one socket per requested env-var name, capture the kernel-assigned port, then close all
// sockets so the sidecar can rebind them. There's an inherent (but small) race between close and
// rebind — see the file-level comment.
async function reservePorts(hostname, envVarNames) {
  const servers = await Promise.all(
    envVarNames.map((_) => getListeningServer(hostname))
  );
  const ports = Object.fromEntries(
    envVarNames.map((envVar, i) => [envVar, servers[i].address().port])
  );
  await Promise.all(servers.map(closeServer));
  return ports;
}

function waitForListening(port, hostname) {
  const { promise, resolve, reject } = Promise.withResolvers();
  const interval = setInterval(() => {
    const conn = net
      .connect(port, hostname, () => {
        conn.destroy();
        clearInterval(interval);
        resolve();
      })
      .once('error', (err) => console.log('waiting for sidecar...', err.code));
  }, CONNECT_POLL_INTERVAL_MS);
  return promise;
}

function runSidecar(cmd) {
  const { promise, resolve } = Promise.withResolvers();
  const proc = child_process.spawn(cmd, {
    shell: true,
    stdio: ['inherit', 'inherit', 'inherit'],
  });
  proc.once('exit', resolve);
  return { promise, proc };
}

async function runPreallocateMode({
  hostname,
  portsToAssign,
  sidecarCommand,
  testArgv,
}) {
  const ports = await reservePorts(hostname, portsToAssign);
  // Export the ports for the sidecar to read before we spawn it.
  Object.assign(process.env, ports);
  const sidecar = runSidecar(sidecarCommand);

  const allListening = Promise.all(
    Object.values(ports).map((port) => waitForListening(port, hostname))
  );
  // If the sidecar dies before all ports come up, surface that as a failure instead of waiting
  // for the per-port timeout.
  const sidecarDied = sidecar.promise.then((code) => {
    throw new Error(`sidecar exited prematurely with code ${code}`);
  });
  await Promise.race([allListening, sidecarDied]);

  process.exitCode = child_process.spawnSync(testArgv[0], testArgv.slice(1), {
    stdio: ['inherit', 'inherit', 'inherit'],
  }).status;

  sidecar.proc.kill();
  await sidecar.promise;
}

// === Hostname assignment ====================================================

function getRandomLoopbackAddress() {
  // Pick a random address in 127.0.0.0/8.
  // Range is chosen not to use network address, gateway address, or broadcast address.

  // TODO: There is still a faint possibility of collision. We could use OS-specific APIs to see
  // if a potential address is in use.
  return `127.${crypto.randomInt(2, 255)}.${crypto.randomInt(2, 255)}.${crypto.randomInt(2, 255)}`;
}

function canUseRandomAddress() {
  if (process.env.RANDOMIZE_IP === 'false') {
    // Test explicitly disabled randomization
    return false;
  }

  switch (process.platform) {
    case 'linux':
      return true;

    case 'win32':
      return true;

    case 'darwin':
      // Address randomization works on macOS but requires sudo, so we'll only
      // use it in CI for dev convenience.
      return process.env.CI === 'true';

    default:
      throw new Error(
        `Address randomization strategy not known for ${process.platform}`
      );
  }
}

function assignLoopbackAddress() {
  if (!canUseRandomAddress()) {
    return '127.0.0.1';
  }

  const randomAddress = getRandomLoopbackAddress();
  if (process.platform === 'darwin') {
    // On macOS, we need to explicitly assign this IP to the loopback interface
    child_process.spawnSync(
      'sudo',
      ['/sbin/ifconfig', 'lo0', 'alias', randomAddress],
      { stdio: 'inherit' }
    );
  }

  return randomAddress;
}

// === Entrypoint =============================================================

const portMode = process.env.SIDECAR_PORT_MODE ?? 'report';
const portsToAssign = process.env.PORTS_TO_ASSIGN.split(',');
const sidecarCommand = process.env.SIDECAR_COMMAND;
const testArgv = process.argv.slice(2); // Shift off the stuff Node puts in argv

const hostname = assignLoopbackAddress();
process.env.SIDECAR_HOSTNAME = hostname;

if (portMode === 'preallocate') {
  await runPreallocateMode({
    hostname,
    portsToAssign,
    sidecarCommand,
    testArgv,
  });
} else if (portMode === 'report') {
  await runReportMode({ portsToAssign, sidecarCommand, testArgv });
} else {
  throw new Error(`unknown SIDECAR_PORT_MODE: ${portMode}`);
}
