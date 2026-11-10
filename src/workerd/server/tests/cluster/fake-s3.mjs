// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// The S3 calls a bucket cluster makes, for tests: GET, PUT (conditional on
// If-Match or If-None-Match: *), DELETE, and ListObjectsV2 (with a delimiter,
// common prefixes only), path-style (`/<bucket>/<key>`). It runs in the test's
// own process, so that it answers at once however slow the instances under
// test are: their leases are short in tests, and a bucket that lags their
// renewals fences them. A conditional write checks and writes with nothing in
// between, so it is atomic. Requests are not authenticated.
//
// A test arms faults with `POST /_faults` and a JSON array: each delays the
// requests whose key starts with `prefix` (and does not end with `except`)
// and whose method is `method` by `delayMs`, then answers `status` instead if
// given, for the next `times` of them. `GET /_faults` lists them, with `hits`.

import { createServer } from 'node:http';
import { setTimeout as sleep } from 'node:timers/promises';

const PAGE = 1000;

function escape(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

export class FakeS3Server {
  #server;
  // Bumped by `reset`: a request a fault delayed across it answers 503, so
  // that it cannot reach the next test's bucket: it lists, counts and
  // changes nothing.
  #generation = 0;
  #next = 0;
  // Key to `{ etag, body }`.
  #objects = new Map();
  // Requests per object key, for tests that bound a cluster's bucket traffic.
  #requests = new Map();
  // Body bytes served per object key.
  #served = new Map();
  #faults = [];

  constructor() {
    this.#server = createServer((req, res) => {
      this.#handle(req, res).catch((err) => res.destroy(err));
    });
  }

  async listen() {
    await new Promise((resolve, reject) => {
      this.#server.once('error', reject);
      this.#server.listen(0, '127.0.0.1', resolve);
    });
    // The test process exits once its tests end, with this still listening.
    this.#server.unref();
  }

  get port() {
    return this.#server.address().port;
  }

  // Empties the bucket and disarms its faults, for the next test.
  reset() {
    this.#generation++;
    this.#objects.clear();
    this.#requests.clear();
    this.#served.clear();
    this.#faults = [];
  }

  async #handle(req, res) {
    const url = new URL(req.url, 'http://s3');
    const headers = { date: new Date().toUTCString() };
    const send = (status, body = null, extra = {}) => {
      res.writeHead(status, { ...headers, ...extra });
      res.end(body);
    };
    if (url.pathname === '/_requests') {
      const key = url.searchParams.get('key');
      return send(200, JSON.stringify(this.#requests.get(key) ?? 0));
    }
    if (url.pathname === '/_served') {
      const key = url.searchParams.get('key');
      return send(200, JSON.stringify(this.#served.get(key) ?? 0));
    }
    if (url.pathname === '/_faults') {
      if (req.method === 'POST') {
        const armed = JSON.parse(await readBody(req));
        this.#faults = armed.map((f) => ({ ...f, hits: 0 }));
      }
      return send(200, JSON.stringify(this.#faults));
    }
    const path = decodeURIComponent(url.pathname).slice(1);
    const slash = path.indexOf('/');
    const key = slash < 0 ? '' : path.slice(slash + 1);
    const generation = this.#generation;

    const fault = this.#faults.find(
      (f) =>
        f.hits < f.times &&
        f.method === req.method &&
        key.startsWith(f.prefix) &&
        !(f.except !== undefined && key.endsWith(f.except))
    );
    if (fault !== undefined) {
      fault.hits++;
      await sleep(fault.delayMs ?? 0);
      if (generation !== this.#generation) return send(503);
      if (fault.status !== undefined) return send(fault.status);
    }

    if (req.method === 'GET' && key === '') {
      return send(200, this.#list(url.searchParams));
    }

    const counted = key.slice(key.indexOf('/') + 1);
    this.#requests.set(counted, (this.#requests.get(counted) ?? 0) + 1);
    const body = req.method === 'PUT' ? await readBody(req) : null;
    if (generation !== this.#generation) return send(503);
    const object = this.#objects.get(key);
    switch (req.method) {
      case 'GET':
        if (!object) return send(404);
        this.#served.set(
          counted,
          (this.#served.get(counted) ?? 0) + object.body.length
        );
        return send(200, object.body, { etag: object.etag });
      case 'PUT': {
        const ifMatch = req.headers['if-match'];
        const ifNoneMatch = req.headers['if-none-match'];
        if (
          (ifNoneMatch === '*' && object) ||
          (ifMatch !== undefined && object?.etag !== ifMatch)
        ) {
          return send(412);
        }
        const etag = `"${Date.now()}-${this.#next++}"`;
        this.#objects.set(key, { etag, body });
        return send(200, null, { etag });
      }
      case 'DELETE':
        this.#objects.delete(key);
        return send(204);
    }
    return send(405);
  }

  // A ListObjectsV2 answer.
  #list(params) {
    const prefix = params.get('prefix') ?? '';
    const keys = [...this.#objects.keys()]
      .filter((k) => k.startsWith(prefix))
      .sort();
    // With a delimiter, one unpaged answer: tests' buckets are small.
    const delimiter = params.get('delimiter');
    if (delimiter !== null) {
      const common = new Set();
      for (const k of keys) {
        const end = k.indexOf(delimiter, prefix.length);
        if (end >= 0) common.add(k.slice(0, end + delimiter.length));
      }
      return (
        '<ListBucketResult><IsTruncated>false</IsTruncated>' +
        [...common]
          .map(
            (p) =>
              `<CommonPrefixes><Prefix>${escape(p)}</Prefix></CommonPrefixes>`
          )
          .join('') +
        '</ListBucketResult>'
      );
    }
    const after = params.get('continuation-token');
    const listed = keys.filter((k) => after === null || k > after);
    const page = listed.slice(0, PAGE);
    const truncated = listed.length > PAGE;
    return (
      '<ListBucketResult>' +
      `<IsTruncated>${truncated}</IsTruncated>` +
      page
        .map(
          (k) =>
            `<Contents><Key>${escape(k)}</Key>` +
            `<Size>${this.#objects.get(k).body.length}</Size></Contents>`
        )
        .join('') +
      (truncated
        ? `<NextContinuationToken>${escape(page.at(-1))}</NextContinuationToken>`
        : '') +
      '</ListBucketResult>'
    );
  }
}

async function readBody(req) {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  return Buffer.concat(chunks);
}
