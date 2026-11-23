// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// The S3 calls a bucket cluster and its R2 binding make, for tests,
// path-style (`/<bucket>/<key>`): HEAD; GET, whole or of a `Range` (206, or
// 416 `InvalidRange`); PUT (conditional on If-Match or If-None-Match: *);
// DELETE; ListObjectsV2 (prefix, start-after, max-keys, delimiter,
// continuation-token); and multipart uploads: initiate (`POST ?uploads`),
// upload a part (`PUT ?partNumber&uploadId`), ListParts (`GET ?uploadId`),
// complete (`POST ?uploadId`) and abort (`DELETE ?uploadId`). It runs in the
// test's own process, so that it answers at once however slow the instances
// under test are: their leases are short in tests, and a bucket that lags
// their renewals fences them. A conditional write checks and writes with
// nothing in between, so it is atomic. An object's ETag is the MD5 of its
// body, as S3's is for an object written in one piece, or of its parts' MD5s
// with `-<parts>`, as S3's is for a multipart one. An object keeps the content
// headers and user metadata its PUT or initiate sent, and answers them to a
// HEAD or GET. Errors answer S3's `<Error><Code>` body, whose `Code` the
// client reads. Requests are not authenticated.
//
// A test arms faults with `POST /_faults` and a JSON array: each delays the
// requests whose key starts with `prefix` (and does not end with `except`)
// and whose method is `method` by `delayMs`, then answers `status` instead if
// given, for the next `times` of them; with `after`, it serves the request
// first and answers `status` instead of its answer. A fault with `via`
// matches only the requests of a client whose endpoint has the path
// `/_via/<via>`, which is not part of the request's path otherwise.
// `GET /_faults` lists them, with `hits`.
// `GET /_rejected?key=` counts the writes to a key whose precondition failed.
//
// With the dialect "gcs", it speaks Cloud Storage's XML API instead: an
// object's version is its generation, unique to each write and answered in
// `x-goog-generation`, and a PUT is conditional on
// `x-goog-if-generation-match` (0: the object must not exist), not on If-Match
// or If-None-Match, which Cloud Storage ignores on a PUT. User metadata is
// `x-goog-meta-*`, and HEAD and GET answer `x-goog-stored-content-length`.
// Multipart uploads are as S3's, as Cloud Storage's XML API mirrors them, but
// for the `x-goog-generation` a completion answers. Every request needs
// a bearer token that has not expired, unless it carries `x-fake-admin`.
// Tokens come from:
//   - GET /computeMetadata/v1/instance/service-accounts/default/token, the
//     metadata server's endpoint, with `Metadata-Flavor: Google`, a header
//     its answers carry too;
//   - POST /token, the JWT bearer grant of a service account's key: the JWT
//     must be signed by the key a test set, for the storage scope alone, now
//     and for an hour.
// `POST /_gcs` with `{ expiresIn, email, jwk }` sets the tokens' lifetime in
// seconds and the service account; `GET /_gcs` counts the tokens issued
// (`metadata`, `jwt`) and the requests refused for a token (`refused`).

import { createHash, createPublicKey, randomUUID, verify } from 'node:crypto';
import { createServer } from 'node:http';
import { setTimeout as sleep } from 'node:timers/promises';

const METADATA_TOKEN =
  '/computeMetadata/v1/instance/service-accounts/default/token';
// The scope a gs:// bucket's service account key asks for.
const STORAGE_SCOPE = 'https://www.googleapis.com/auth/devstorage.read_write';

const PAGE = 1000;
// S3's least part but the last.
const MIN_PART = 5 * 1024 * 1024;
// The content headers an object keeps from its PUT or initiate.
const CONTENT = [
  'content-type',
  'content-language',
  'content-disposition',
  'content-encoding',
  'cache-control',
];

function escape(s) {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

function unescape(s) {
  return s
    .replace(/&quot;/g, '"')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&amp;/g, '&');
}

function md5(data) {
  return createHash('md5').update(data).digest();
}

// The text of each `<name>` element in `xml`.
function elements(xml, name) {
  const re = new RegExp(`<${name}>([\\s\\S]*?)</${name}>`, 'g');
  return [...xml.matchAll(re)].map((m) => m[1]);
}

export class FakeS3Server {
  #server;
  #gcs;
  // Bumped by `reset`: a request a fault delayed across it answers 503, so
  // that it cannot reach the next test's bucket: it lists, counts and
  // changes nothing.
  #generation = 0;
  #next = 0;
  // Key to `{ etag, generation, body, headers, lastModified }`, `headers`
  // the content headers and user metadata as sent, in order.
  #objects = new Map();
  // Upload ID to `{ key, headers, parts }`, `parts` part number to
  // `{ etag, body }`.
  #uploads = new Map();
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
    this.#uploads.clear();
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
    const [, via, rest] = /^\/_via\/([^/]+)(.*)$/.exec(url.pathname) ?? [];
    if (via !== undefined) url.pathname = rest || '/';
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
        (f.via === undefined || f.via === via) &&
        key.startsWith(f.prefix) &&
        !(f.except !== undefined && key.endsWith(f.except))
    );
    if (fault !== undefined) {
      fault.hits++;
      await sleep(fault.delayMs ?? 0);
      if (generation !== this.#generation) return send(503);
      if (fault.status !== undefined && !fault.after) return send(fault.status);
    }
    const reply = fault?.after ? () => send(fault.status) : send;
    const fail = (status, code, message = code) =>
      reply(
        status,
        `<Error><Code>${code}</Code><Message>${escape(message)}</Message>` +
          '</Error>',
        { 'content-type': 'application/xml' }
      );
    const xml = (body, extra = {}) =>
      reply(200, body, { 'content-type': 'application/xml', ...extra });

    if (req.method === 'GET' && key === '') {
      return xml(this.#list(url.searchParams));
    }

    const counted = key.slice(key.indexOf('/') + 1);
    this.#requests.set(counted, (this.#requests.get(counted) ?? 0) + 1);
    const body =
      req.method === 'PUT' || req.method === 'POST'
        ? await readBody(req)
        : null;
    if (generation !== this.#generation) return send(503);
    const query = url.searchParams;
    const id = query.get('uploadId');
    if (id !== null) {
      const upload = this.#uploads.get(id);
      if (upload?.key !== key) return fail(404, 'NoSuchUpload', id);
      return this.#multipart(req.method, query, upload, id, key, body, {
        reply,
        fail,
        xml,
      });
    }
    // S3's limit on user metadata, names and values.
    const meta = this.#described(req).filter(([n]) => n.startsWith('x-amz-'));
    const metaSize = meta.reduce(
      (n, [name, v]) => n + Buffer.byteLength(name.slice(11) + v),
      0
    );
    if (metaSize > 2048 && ['PUT', 'POST'].includes(req.method)) {
      return fail(400, 'MetadataTooLarge', key);
    }
    if (req.method === 'POST' && query.has('uploads')) {
      const id = randomUUID();
      this.#uploads.set(id, { key, headers: this.#described(req), parts: [] });
      return xml(
        '<InitiateMultipartUploadResult>' +
          `<Bucket>${escape(path.slice(0, slash))}</Bucket>` +
          `<Key>${escape(key)}</Key><UploadId>${id}</UploadId>` +
          '</InitiateMultipartUploadResult>'
      );
    }
    const object = this.#objects.get(key);
    switch (req.method) {
      case 'HEAD':
        if (!object) return reply(404);
        return reply(200, null, {
          ...this.#answered(object),
          'content-length': object.body.length,
        });
      case 'GET': {
        if (!object) return fail(404, 'NoSuchKey', key);
        const size = object.body.length;
        const range = byteRange(req.headers.range, size);
        if (range === 'unsatisfiable') {
          return fail(416, 'InvalidRange', req.headers.range);
        }
        const [first, last] = range ?? [0, size - 1];
        const slice = object.body.subarray(first, last + 1);
        this.#served.set(
          counted,
          (this.#served.get(counted) ?? 0) + slice.length
        );
        const extra = {
          ...this.#answered(object),
          'content-length': slice.length,
        };
        if (range === null) return reply(200, slice, extra);
        extra['content-range'] = `bytes ${first}-${last}/${size}`;
        return reply(206, slice, extra);
      }
      case 'PUT': {
        if (!this.#holds(req, object)) {
          this.#rejected.set(key, (this.#rejected.get(key) ?? 0) + 1);
          return fail(412, 'PreconditionFailed');
        }
        const etag = `"${md5(body).toString('hex')}"`;
        const stored = this.#store(key, etag, body, this.#described(req));
        return reply(200, null, this.#version(stored));
      }
      case 'DELETE':
        this.#objects.delete(key);
        return reply(204);
    }
    return send(405);
  }

  // A call on the multipart upload `id` of `key`, which exists.
  #multipart(method, query, upload, id, key, body, { reply, fail, xml }) {
    switch (method) {
      case 'PUT': {
        const n = Number(query.get('partNumber'));
        if (!(Number.isInteger(n) && n >= 1 && n <= 10000)) {
          return fail(400, 'InvalidArgument', `partNumber ${n}`);
        }
        const etag = `"${md5(body).toString('hex')}"`;
        upload.parts[n] = { etag, body };
        return reply(200, null, { etag });
      }
      case 'GET': {
        const marker = Number(query.get('part-number-marker') ?? 0);
        const max = Number(query.get('max-parts') ?? PAGE);
        const listed = upload.parts
          .map((part, n) => ({ ...part, n }))
          .filter((part) => part && part.n > marker);
        const page = listed.slice(0, max);
        return xml(
          '<ListPartsResult>' +
            `<Key>${escape(key)}</Key><UploadId>${id}</UploadId>` +
            `<PartNumberMarker>${marker}</PartNumberMarker>` +
            `<NextPartNumberMarker>${page.at(-1)?.n ?? 0}` +
            '</NextPartNumberMarker>' +
            `<MaxParts>${max}</MaxParts>` +
            `<IsTruncated>${listed.length > max}</IsTruncated>` +
            page
              .map(
                (p) =>
                  `<Part><PartNumber>${p.n}</PartNumber>` +
                  `<ETag>${escape(p.etag)}</ETag>` +
                  `<Size>${p.body.length}</Size></Part>`
              )
              .join('') +
            '</ListPartsResult>'
        );
      }
      case 'POST': {
        const named = elements(body.toString(), 'Part').map((part) => ({
          n: Number(elements(part, 'PartNumber')[0]),
          etag: unescape(elements(part, 'ETag')[0] ?? '').replace(/"/g, ''),
        }));
        if (named.length === 0) return fail(400, 'MalformedXML');
        // The order is checked first, as S3 does, and then each part.
        const i = named.findIndex((p, i) => i > 0 && !(p.n > named[i - 1].n));
        if (i > 0) return fail(400, 'InvalidPartOrder', `part ${named[i].n}`);
        const parts = [];
        for (const [i, { n, etag }] of named.entries()) {
          const part = upload.parts[n];
          if (part?.etag !== `"${etag}"`) {
            return fail(400, 'InvalidPart', `part ${n}`);
          }
          if (i < named.length - 1 && part.body.length < MIN_PART) {
            return fail(400, 'EntityTooSmall', `part ${n}`);
          }
          parts.push(part);
        }
        const digests = parts.map((p) =>
          Buffer.from(p.etag.slice(1, -1), 'hex')
        );
        const digest = md5(Buffer.concat(digests)).toString('hex');
        const etag = `"${digest}-${parts.length}"`;
        const joined = Buffer.concat(parts.map((p) => p.body));
        this.#uploads.delete(id);
        const stored = this.#store(key, etag, joined, upload.headers);
        // S3 answers the ETag in the body alone.
        const version = this.#gcs
          ? { 'x-goog-generation': stored.generation }
          : {};
        return xml(
          '<CompleteMultipartUploadResult><Location></Location>' +
            `<Bucket></Bucket><Key>${escape(key)}</Key>` +
            `<ETag>${escape(etag)}</ETag></CompleteMultipartUploadResult>`,
          version
        );
      }
      case 'DELETE':
        this.#uploads.delete(id);
        return reply(204);
    }
    return fail(405, 'MethodNotAllowed');
  }

  // Stores a write of `key`, answering the object.
  #store(key, etag, body, headers) {
    // Decimal, unique, and never 0.
    const generation = `${Date.now()}${String(this.#next++).padStart(6, '0')}`;
    const stored = {
      etag,
      generation,
      body,
      headers,
      lastModified: new Date(),
    };
    this.#objects.set(key, stored);
    return stored;
  }

  // A write's content headers and user metadata, as sent and in order.
  #described(req) {
    const meta = this.#gcs ? 'x-goog-meta-' : 'x-amz-meta-';
    const out = [];
    for (let i = 0; i < req.rawHeaders.length; i += 2) {
      const name = req.rawHeaders[i].toLowerCase();
      if (CONTENT.includes(name) || name.startsWith(meta)) {
        out.push([name, req.rawHeaders[i + 1]]);
      }
    }
    return out;
  }

  // The headers a HEAD or GET of `object` answers, but its length.
  #answered(object) {
    return {
      ...Object.fromEntries(object.headers),
      ...this.#version(object),
      'last-modified': object.lastModified.toUTCString(),
      ...(this.#gcs
        ? { 'x-goog-stored-content-length': object.body.length }
        : {}),
    };
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

  // A ListObjectsV2 answer. Keys and common prefixes are merged in order
  // and paged together, delimiter or not; a token is the last of a page.
  #list(params) {
    const prefix = params.get('prefix') ?? '';
    const delimiter = params.get('delimiter') || null;
    const token = params.get('continuation-token');
    // A token supersedes start-after, as on S3.
    const after = token === null ? params.get('start-after') : null;
    const max = Number(params.get('max-keys') ?? PAGE);
    const keys = [...this.#objects.keys()]
      .filter((k) => k.startsWith(prefix) && (after === null || k > after))
      .sort();
    // Each a key, or a common prefix: one entry for all the keys under it.
    const listed = [];
    for (const k of keys) {
      const end = delimiter === null ? -1 : k.indexOf(delimiter, prefix.length);
      const name = end < 0 ? k : k.slice(0, end + delimiter.length);
      if (token !== null && name <= token) continue;
      if (listed.at(-1)?.name === name) continue;
      listed.push({ name, common: end >= 0 });
    }
    const page = listed.slice(0, max);
    const truncated = listed.length > page.length;
    return (
      '<ListBucketResult>' +
      `<Prefix>${escape(prefix)}</Prefix><KeyCount>${page.length}</KeyCount>` +
      `<MaxKeys>${max}</MaxKeys><IsTruncated>${truncated}</IsTruncated>` +
      page
        .map(({ name, common }) => {
          if (common) {
            return (
              `<CommonPrefixes><Prefix>${escape(name)}</Prefix>` +
              '</CommonPrefixes>'
            );
          }
          const o = this.#objects.get(name);
          return (
            `<Contents><Key>${escape(name)}</Key>` +
            `<LastModified>${o.lastModified.toISOString()}</LastModified>` +
            `<ETag>${escape(o.etag)}</ETag><Size>${o.body.length}</Size>` +
            (this.#gcs ? `<Generation>${o.generation}</Generation>` : '') +
            '</Contents>'
          );
        })
        .join('') +
      (truncated
        ? '<NextContinuationToken>' +
          escape(page.at(-1).name) +
          '</NextContinuationToken>'
        : '') +
      '</ListBucketResult>'
    );
  }
}

// A `Range` header's first and last byte of an object of `size`: null for
// none or one S3 ignores, as it does a malformed one, so the whole object;
// 'unsatisfiable' for one past its end.
function byteRange(header, size) {
  const m = header?.match(/^bytes=(\d*)-(\d*)$/);
  if (!m || (m[1] === '' && m[2] === '')) return null;
  if (m[1] === '') {
    const n = Number(m[2]);
    return n === 0 || size === 0
      ? 'unsatisfiable'
      : [Math.max(0, size - n), size - 1];
  }
  const first = Number(m[1]);
  const last = m[2] === '' ? size - 1 : Math.min(Number(m[2]), size - 1);
  if (m[2] !== '' && Number(m[2]) < first) return null;
  return first >= size ? 'unsatisfiable' : [first, last];
}

async function readBody(req) {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  return Buffer.concat(chunks);
}
