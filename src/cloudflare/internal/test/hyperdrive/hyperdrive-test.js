// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';

const HOST_PATTERN = /^[0-9a-f]{32}\.hyperdrive\.local$/;
const CREDENTIAL_PATTERN = /^[0-9a-f]{32}$/;

const PROPERTY_NAMES = [
  'database',
  'user',
  'password',
  'scheme',
  'connectionString',
  'host',
  'port',
];

async function readLine(reader) {
  const decoder = new TextDecoder();
  let text = '';
  while (!text.includes('\n')) {
    const { value, done } = await reader.read();
    if (done) break;
    text += decoder.decode(value, { stream: true });
  }
  const newline = text.indexOf('\n');
  return { line: text.slice(0, newline), rest: text.slice(newline + 1) };
}

export const postgresProperties = {
  test(_ctrl, env) {
    const hd = env.HYPERDRIVE_PG;
    assert.match(hd.database, CREDENTIAL_PATTERN);
    assert.match(hd.user, CREDENTIAL_PATTERN);
    assert.strictEqual(hd.scheme, 'postgresql');
    assert.strictEqual(hd.port, 5432);
    assert.match(hd.host, HOST_PATTERN);
    // The host is stable across reads.
    assert.strictEqual(hd.host, hd.host);
  },
};

export const mysqlProperties = {
  test(_ctrl, env) {
    const hd = env.HYPERDRIVE_MYSQL;
    assert.match(hd.database, CREDENTIAL_PATTERN);
    assert.match(hd.user, CREDENTIAL_PATTERN);
    assert.strictEqual(hd.scheme, 'mysql');
    assert.strictEqual(hd.port, 3306);
    assert.match(hd.host, HOST_PATTERN);
  },
};

// database, user, and password are independent random placeholders: the real credentials only
// reach the Hyperdrive RPC worker. They are not embedded in the host.
export const generatedCredentials = {
  test(_ctrl, env) {
    const pg = env.HYPERDRIVE_PG;
    const mysql = env.HYPERDRIVE_MYSQL;
    for (const name of ['database', 'user', 'password']) {
      assert.ok(!pg.host.includes(pg[name]), name);
      assert.match(pg[name], CREDENTIAL_PATTERN, name);
      assert.match(mysql[name], CREDENTIAL_PATTERN, name);
      // Generated once per binding instance and then stable.
      assert.strictEqual(pg[name], pg[name], name);
      // Independent per binding instance.
      assert.notStrictEqual(pg[name], mysql[name], name);
    }
    // Independent of each other.
    assert.strictEqual(new Set([pg.database, pg.user, pg.password]).size, 3);
  },
};

export const postgresConnectionString = {
  test(_ctrl, env) {
    const hd = env.HYPERDRIVE_PG;
    assert.strictEqual(
      hd.connectionString,
      `postgresql://${hd.user}:${hd.password}@${hd.host}:5432/${hd.database}?sslmode=disable`
    );

    const url = new URL(hd.connectionString);
    assert.strictEqual(url.protocol, 'postgresql:');
    assert.strictEqual(url.username, hd.user);
    assert.strictEqual(url.password, hd.password);
    assert.strictEqual(url.hostname, hd.host);
    assert.strictEqual(url.port, '5432');
    assert.strictEqual(url.pathname, `/${hd.database}`);
    assert.strictEqual(url.searchParams.get('sslmode'), 'disable');
  },
};

export const mysqlConnectionString = {
  test(_ctrl, env) {
    const hd = env.HYPERDRIVE_MYSQL;
    assert.strictEqual(
      hd.connectionString,
      `mysql://${hd.user}:${hd.password}@${hd.host}:3306/${hd.database}?ssl-mode=disabled`
    );

    // Drivers parse the generated credentials back out unchanged; hex needs no URL escaping.
    const url = new URL(hd.connectionString);
    assert.strictEqual(url.protocol, 'mysql:');
    assert.strictEqual(url.username, hd.user);
    assert.strictEqual(url.password, hd.password);
    assert.strictEqual(url.hostname, hd.host);
    assert.strictEqual(url.port, '3306');
    assert.strictEqual(url.pathname, `/${hd.database}`);
    assert.strictEqual(url.searchParams.get('ssl-mode'), 'disabled');
  },
};

export const propertyShape = {
  test(_ctrl, env) {
    const hd = env.HYPERDRIVE_PG;

    // Data properties are own, enumerable, read-only accessors; connect is not enumerable.
    assert.deepStrictEqual(Object.keys(hd), PROPERTY_NAMES);
    for (const name of PROPERTY_NAMES) {
      const desc = Object.getOwnPropertyDescriptor(hd, name);
      assert.ok(desc, name);
      assert.strictEqual(desc.enumerable, true, name);
      assert.strictEqual(desc.configurable, false, name);
      assert.strictEqual(typeof desc.get, 'function', name);
      assert.strictEqual(desc.set, undefined, name);
    }

    const connectDesc = Object.getOwnPropertyDescriptor(hd, 'connect');
    assert.ok(connectDesc);
    assert.strictEqual(typeof connectDesc.value, 'function');
    assert.strictEqual(connectDesc.enumerable, false);
    assert.strictEqual(connectDesc.writable, false);
    assert.strictEqual(connectDesc.configurable, false);

    // Modules are strict, so writes to read-only properties throw.
    assert.throws(() => {
      hd.password = 'nope';
    }, TypeError);
    assert.throws(() => {
      hd.connect = () => {};
    }, TypeError);
    assert.throws(() => {
      delete hd.host;
    }, TypeError);

    // connect is a single stable function that can't be redefined.
    assert.strictEqual(hd.connect, hd.connect);
    assert.throws(
      () => Object.defineProperty(hd, 'connect', { value: () => {} }),
      TypeError
    );

    // The inner fetcher is not exposed: no fetch(), no RPC wildcard, no private helpers.
    assert.strictEqual(hd.fetch, undefined);
    assert.strictEqual(hd.unexpectedRpcMethod, undefined);
    assert.strictEqual('getPassword' in hd, false);
    assert.strictEqual('fetcher' in hd, false);

    // Private state isn't leaked through JSON or spread.
    assert.deepStrictEqual(Object.keys({ ...hd }), PROPERTY_NAMES);
    assert.deepStrictEqual(Object.keys(JSON.parse(JSON.stringify(hd))), [
      ...PROPERTY_NAMES,
    ]);
  },
};

async function testConnect(hd) {
  // connect() is usable detached from the binding.
  const { connect } = hd;
  const socket = connect();
  await socket.opened;

  const writer = socket.writable.getWriter();
  const reader = socket.readable.getReader();

  // The mock reports the authority the socket was opened against.
  const { line, rest } = await readLine(reader);
  assert.strictEqual(line, `${hd.host}:${hd.port}`);

  // Round-trip data through the socket.
  const payload = 'SELECT 1;';
  await writer.write(new TextEncoder().encode(payload));
  const decoder = new TextDecoder();
  let echoed = rest;
  while (echoed.length < payload.length) {
    const { value, done } = await reader.read();
    if (done) break;
    echoed += decoder.decode(value, { stream: true });
  }
  assert.strictEqual(echoed, payload);

  await writer.close();
  reader.releaseLock();
  await socket.close();
}

export const postgresConnect = {
  async test(_ctrl, env) {
    await testConnect(env.HYPERDRIVE_PG);
  },
};

export const mysqlConnect = {
  async test(_ctrl, env) {
    await testConnect(env.HYPERDRIVE_MYSQL);
  },
};
