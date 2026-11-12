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
// `GET /_rejected?key=` counts the writes to a key whose precondition failed.
//
// With the dialect "gcs", it speaks Cloud Storage's XML API instead: an
// object's version is its generation, answered in `x-goog-generation`, and a
// PUT is conditional on `x-goog-if-generation-match` (0: the object must not
// exist), not on If-Match or If-None-Match, which Cloud Storage ignores on a
// PUT. Every request needs a bearer token that has not expired, unless it
// carries `x-fake-admin`. Tokens come from:
//   - GET /computeMetadata/v1/instance/service-accounts/default/token, the
//     metadata server's endpoint, with `Metadata-Flavor: Google`, a header
//     its answers carry too;
//   - POST /token, the JWT bearer grant of a service account's key: the JWT
//     must be signed by the key a test set, for the storage scope alone, now
//     and for an hour.
// `POST /_gcs` with `{ expiresIn, email, jwk }` sets the tokens' lifetime in
// seconds and the service account; `GET /_gcs` counts the tokens issued
// (`metadata`, `jwt`) and the requests refused for a token (`refused`).

import { createPublicKey, randomUUID, verify } from 'node:crypto';
import { createServer } from 'node:http';
import { setTimeout as sleep } from 'node:timers/promises';

const METADATA_TOKEN =
  '/computeMetadata/v1/instance/service-accounts/default/token';
// The scope a gs:// bucket's service account key asks for.
const STORAGE_SCOPE = 'https://www.googleapis.com/auth/devstorage.read_write';

const PAGE = 1000;

function escape(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

export class FakeS3Server {
  #server;
  #gcs;
  // Bumped by `reset`: a request a fault delayed across it answers 503, so
  // that it cannot reach the next test's bucket: it lists, counts and
  // changes nothing.
  #generation = 0;
  #next = 0;
  // Key to `{ etag, generation, body }`.
  #objects = new Map();
  // Writes per key whose precondition failed.
  #rejected = new Map();
  // Cloud Storage's tokens: each one's expiry, in ms since the epoch.
  #tokens = new Map();
  #account;
  #issued;
  // Requests per object key, for tests that bound a cluster's bucket traffic.
  #requests = new Map();
  // Body bytes served per object key.
  #served = new Map();
  #faults = [];

  // `dialect` is "s3" or "gcs".
  constructor({ dialect = 's3' } = {}) {
    this.#gcs = dialect === 'gcs';
    this.reset();
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
    this.#rejected.clear();
    this.#requests.clear();
    this.#served.clear();
    this.#faults = [];
    this.#tokens.clear();
    this.#account = { expiresIn: 3600, email: null, jwk: null };
    this.#issued = { metadata: 0, jwt: 0, refused: 0 };
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
    if (url.pathname === '/_rejected') {
      const key = url.searchParams.get('key');
      return send(200, JSON.stringify(this.#rejected.get(key) ?? 0));
    }
    if (this.#gcs) {
      const answered = await this.#authorize(req, url, send);
      if (answered) return;
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
        return send(200, object.body, this.#version(object));
      case 'PUT': {
        if (!this.#holds(req, object)) {
          this.#rejected.set(key, (this.#rejected.get(key) ?? 0) + 1);
          return send(412);
        }
        const etag = `"${Date.now()}-${this.#next}"`;
        // Decimal, unique, and never 0.
        const generation = `${Date.now()}${String(this.#next++).padStart(6, '0')}`;
        const stored = { etag, generation, body };
        this.#objects.set(key, stored);
        return send(200, null, this.#version(stored));
      }
      case 'DELETE':
        this.#objects.delete(key);
        return send(204);
    }
    return send(405);
  }

  // The headers that name a stored object's version.
  #version(object) {
    return this.#gcs
      ? { etag: object.etag, 'x-goog-generation': object.generation }
      : { etag: object.etag };
  }

  // Whether a PUT's precondition holds for `object`, the stored one if any.
  #holds(req, object) {
    if (this.#gcs) {
      const generation = req.headers['x-goog-if-generation-match'];
      if (generation === undefined) return true;
      return generation === '0'
        ? object === undefined
        : object?.generation === generation;
    }
    const ifMatch = req.headers['if-match'];
    const ifNoneMatch = req.headers['if-none-match'];
    return !(
      (ifNoneMatch === '*' && object) ||
      (ifMatch !== undefined && object?.etag !== ifMatch)
    );
  }

  // Cloud Storage's tokens: answers a token endpoint's request or refuses a
  // request without a valid token, and says whether it did; a request that
  // may go on is not answered.
  async #authorize(req, url, send) {
    const json = (status, value, extra = {}) => {
      send(status, JSON.stringify(value), {
        'content-type': 'application/json',
        ...extra,
      });
      return true;
    };
    if (url.pathname === '/_gcs') {
      if (req.method === 'POST') {
        const account = JSON.parse(await readBody(req));
        this.#account = { ...this.#account, ...account };
      }
      return json(200, this.#issued);
    }
    if (url.pathname === METADATA_TOKEN) {
      const flavor = { 'metadata-flavor': 'Google' };
      if (req.headers['metadata-flavor'] !== 'Google') {
        send(403, 'missing Metadata-Flavor', flavor);
        return true;
      }
      this.#issued.metadata++;
      return json(200, this.#issue(), flavor);
    }
    if (url.pathname === '/token' && req.method === 'POST') {
      const form = new URLSearchParams((await readBody(req)).toString());
      // The audience is the token endpoint as the client named it.
      const audience = `http://${req.headers.host}/token`;
      const error = this.#checkAssertion(form, audience);
      if (error !== null) {
        return json(400, { error: 'invalid_grant', error_description: error });
      }
      this.#issued.jwt++;
      return json(200, this.#issue());
    }
    if (req.headers['x-fake-admin'] !== undefined) return false;
    const token = (req.headers.authorization ?? '').match(/^Bearer (.+)$/)?.[1];
    if (!(this.#tokens.get(token) > Date.now())) {
      this.#issued.refused++;
      send(401);
      return true;
    }
    return false;
  }

  // A new token's answer.
  #issue() {
    const token = `ya29.${randomUUID()}`;
    this.#tokens.set(token, Date.now() + this.#account.expiresIn * 1000);
    return {
      access_token: token,
      expires_in: this.#account.expiresIn,
      token_type: 'Bearer',
    };
  }

  // Why a JWT bearer grant is refused, or null.
  #checkAssertion(form, audience) {
    if (
      form.get('grant_type') !== 'urn:ietf:params:oauth:grant-type:jwt-bearer'
    ) {
      return `grant_type ${form.get('grant_type')}`;
    }
    const [header, claims, signature] = (form.get('assertion') ?? '').split(
      '.'
    );
    if (signature === undefined || this.#account.jwk === null) return 'no JWT';
    const key = createPublicKey({ key: this.#account.jwk, format: 'jwk' });
    const signed = Buffer.from(`${header}.${claims}`);
    const decode = (part) => Buffer.from(part, 'base64url');
    if (!verify('RSA-SHA256', signed, key, decode(signature))) {
      return 'bad signature';
    }
    const { alg } = JSON.parse(decode(header).toString());
    const { iss, aud, scope, iat, exp } = JSON.parse(decode(claims).toString());
    if (alg !== 'RS256') return `alg ${alg}`;
    if (iss !== this.#account.email) return `iss ${iss}`;
    if (aud !== audience) return `aud ${aud}`;
    if (scope !== STORAGE_SCOPE) return `scope ${scope}`;
    // Signed now, for the hour Google allows, within a minute of skew.
    const now = Date.now() / 1000;
    if (!(Math.abs(iat - now) <= 60)) return `iat ${iat}`;
    if (exp !== iat + 3600) return `exp ${exp}`;
    return null;
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
