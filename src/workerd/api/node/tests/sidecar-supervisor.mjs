// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import net from 'node:net';
import child_process from 'node:child_process';

const ANY_PORT = 0;
const CONNECT_POLL_INTERVAL_MS = 500;

function getListeningServer() {
  const { promise, resolve } = Promise.withResolvers();
  const server = net.createServer();
  server.listen(ANY_PORT).once('listening', () => resolve(server));
  return promise;
}

function closeServer(server) {
  const { promise, resolve } = Promise.withResolvers();
  server.close(resolve);
  return promise;
}

async function reservePorts(envVarNames) {
  const servers = await Promise.all(envVarNames.map(getListeningServer));
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

function waitForListening(port) {
  const { promise, resolve, reject } = Promise.withResolvers();
  const interval = setInterval(() => {
    const conn = net
      .connect(port, () => {
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

const portsToAssign = process.env.PORTS_TO_ASSIGN.split(',');
const sidecarCommand = process.env.SIDECAR_COMMAND;
const testArgv = process.argv.slice(2); // Shift off the stuff Node puts in argv

const ports = await reservePorts(portsToAssign);
const sidecar = runSidecar(sidecarCommand);
await Promise.all(Object.values(ports).map(waitForListening));

process.exitCode = child_process.spawnSync(testArgv[0], testArgv.slice(1), {
  stdio: ['inherit', 'inherit', 'inherit'],
}).status;

sidecar.proc.kill();
await sidecar.promise;
