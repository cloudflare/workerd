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
 * communicate (stored in the environment variable `SIDECAR_HOSTNAME`), as well as a set of random
 * port numbers. Each environment variable specified in `sidecar_port_bindings` will be filled in
 * with a random port number.
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
 *                 env vars: PORTS_TO_ASSIGN
 *                             ▼
 * ┌───────────────────────────────────────────────────────┐
 * │                                                       │
 * │                   sidecar supervisor                  ├────────────────────────────────────────────────────────────┐
 * │                                                       │                                                            │
 * └───────────────────────────┬───────────────────────────┘                                                            │
 *                             │                                                                  env vars: SIDECAR_HOSTNAME, SERVER_PORT, ...
 *       env vars: SIDECAR_HOSTNAME, SERVER_PORT, ...                                                                   │
 *                             ▼                                                                                        ▼
 * ┌───────────────────────────────────────────────────────┐                                    ┌──────────────────────────────────────────────┐
 * │                                                       │                                    │                                              │
 * │                        wd-test                        │                                    │                   sidecar                    │
 * │                                                       │                                    │                                              │
 * └───────────────────────────┬───────────────────────────┘                                    └──────────────────────────────────────────────┘
 *                             │                                                                                        ▲
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

const ANY_PORT = 0;
const CONNECT_POLL_INTERVAL_MS = 500;

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

async function reservePorts(hostname, envVarNames) {
  const servers = await Promise.all(
    envVarNames.map((_) => getListeningServer(hostname))
  );
  const ports = Object.fromEntries(
    envVarNames.map((envVar, i) => [envVar, servers[i].address().port])
  );
  Object.assign(process.env, ports);

  // TODO(soon): We need to close the ports we found so sidecarCommand can bind to them.
  // During this time, another unrelated process could end up taking the ports we're using.
  // SO_REUSEPORT is safer but the sidecarCommand must know to use it.
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

const portsToAssign = process.env.PORTS_TO_ASSIGN.split(',');
const sidecarCommand = process.env.SIDECAR_COMMAND;
const testArgv = process.argv.slice(2); // Shift off the stuff Node puts in argv

const hostname = assignLoopbackAddress();
process.env.SIDECAR_HOSTNAME = hostname;

const ports = await reservePorts(hostname, portsToAssign);
const sidecar = runSidecar(sidecarCommand);
await Promise.all(
  Object.values(ports).map((port) => waitForListening(port, hostname))
);

process.exitCode = child_process.spawnSync(testArgv[0], testArgv.slice(1), {
  stdio: ['inherit', 'inherit', 'inherit'],
}).status;

sidecar.proc.kill();
await sidecar.promise;
