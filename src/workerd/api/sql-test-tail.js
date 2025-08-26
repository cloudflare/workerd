import * as assert from 'node:assert';

let invocationPromises = [];
let spans = new Map();

export default {
  async fetch(ctrl, env, ctx) {
    return new Response('');
  },
  async test(ctrl, env, ctx) {
    let received = Array.from(spans.values()).filter(
      (span) => span.name !== 'jsRpcSession'
    );

    const expected = [
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_ingest',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_storage_exec',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
      {
        name: 'durable_object_subrequest',
        closed: true,
      },
      {
        name: 'worker',
        closed: true,
      },
    ];
    await Promise.allSettled(invocationPromises);
    assert.deepStrictEqual(received, expected);

    return new Response('');
  },

  tailStream(event, env, ctx) {
    let resolveFn;
    invocationPromises.push(
      new Promise((resolve, reject) => {
        resolveFn = resolve;
      })
    );

    return (event) => {
      switch (event.event.type) {
        case 'spanOpen':
          // The span ids will change between tests, but Map preserves insertion order
          spans.set(event.spanId, { name: event.event.name });
          break;
        case 'attributes': {
          let span = spans.get(event.spanId);
          for (let { name, value } of event.event.info) {
            span[name] = value;
          }
          spans.set(event.spanId, span);
          break;
        }
        case 'spanClose': {
          let span = spans.get(event.spanId);
          span['closed'] = true;
          spans.set(event.spanId, span);
          break;
        }
        case 'outcome':
          resolveFn();
          break;
      }
    };
  },
};
