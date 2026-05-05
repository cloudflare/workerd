// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';
import {
  invocationPromises,
  spans,
  testTailHandler,
} from 'test:instrumentation-tail';

export default testTailHandler;

export const test = {
  async test() {
    await Promise.allSettled(invocationPromises);
    // filter out non-KV spans including jsRpc calls
    let received = Array.from(spans.values()).filter((span) =>
      span.name.match('kv')
    );

    assert.deepStrictEqual(received, expectedSpans);
  },
};

const expectedSpans = [
  {
    name: 'durable_object_storage_kv_get',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'get',
    'cloudflare.durable_object.kv.query.keys': 'foo',
    'cloudflare.durable_object.kv.query.keys.count': 1n,
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_put',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'put',
    'cloudflare.durable_object.kv.query.keys': 'foo',
    'cloudflare.durable_object.kv.query.keys.count': 1n,
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_get',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'get',
    'cloudflare.durable_object.kv.query.keys': 'foo',
    'cloudflare.durable_object.kv.query.keys.count': 1n,
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_get',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'get',
    'cloudflare.durable_object.kv.query.keys': 'bar',
    'cloudflare.durable_object.kv.query.keys.count': 1n,
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_put',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'put',
    'cloudflare.durable_object.kv.query.keys': 'bar',
    'cloudflare.durable_object.kv.query.keys.count': 1n,
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_get',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'get',
    'cloudflare.durable_object.kv.query.keys': 'bar',
    'cloudflare.durable_object.kv.query.keys.count': 1n,
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_list',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'list',
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_list',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'list',
    'cloudflare.durable_object.kv.query.reverse': true,
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_list',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'list',
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_list',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'list',
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_put',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'put',
    'cloudflare.durable_object.kv.query.keys': 'baz',
    'cloudflare.durable_object.kv.query.keys.count': 1n,
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_list',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'list',
    'cloudflare.durable_object.kv.query.prefix': 'ba',
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_list',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'list',
    'cloudflare.durable_object.kv.query.limit': 1n,
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_list',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'list',
    'cloudflare.durable_object.kv.query.reverse': true,
    'cloudflare.durable_object.kv.query.limit': 1n,
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_list',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'list',
    'cloudflare.durable_object.kv.query.start': 'b',
    'cloudflare.durable_object.kv.query.end': 'c',
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_list',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'list',
    'cloudflare.durable_object.kv.query.start': 'b',
    'cloudflare.durable_object.kv.query.end': 'baz',
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_list',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'list',
    'cloudflare.durable_object.kv.query.start': 'bar',
    'cloudflare.durable_object.kv.query.end': 'c',
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_list',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'list',
    'cloudflare.durable_object.kv.query.startAfter': 'bar',
    'cloudflare.durable_object.kv.query.end': 'c',
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_delete',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'delete',
    'cloudflare.durable_object.kv.query.keys': 'qux',
    'cloudflare.durable_object.kv.query.keys.count': 1n,
    'cloudflare.durable_object.kv.response.deleted_count': 0n,
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_delete',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'delete',
    'cloudflare.durable_object.kv.query.keys': 'bar',
    'cloudflare.durable_object.kv.query.keys.count': 1n,
    'cloudflare.durable_object.kv.response.deleted_count': 1n,
    closed: true,
  },
  {
    name: 'durable_object_storage_kv_list',
    'db.system.name': 'cloudflare-durable-object-sql',
    'db.operation.name': 'list',
    closed: true,
  },
];
