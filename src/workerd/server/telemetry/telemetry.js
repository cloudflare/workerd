// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Telemetry, an OTLP/HTTP collector that keeps what workerd exports in a
// bucket as gzipped NDJSON, which DuckDB queries in place:
//
//   SELECT name, duration_us FROM read_json('s3://<bucket>/<prefix>services/
//     telemetry/traces/**/*.ndjson.gz') WHERE NOT ok
//
// `WORKERD_OTEL=1` adds it (cli/config.rs) as the service `workerd-telemetry`
// that `tracing.otlp` names, with `exportLogs` and `protocol = httpJson`; a
// hand-written config may bind any `bucket` service. The config that runs
// this module (`bundled = "/workerd/telemetry.js"`) is an interface. Its
// bindings:
//
//   STORE  a `bucket` service
//
// It takes OTLP/JSON only. Each POST to /v1/traces or /v1/logs is one file,
// `{traces,logs}/<yyyy>/<mm>/<dd>/<hh>/<unix_ms>-<rand>.ndjson.gz`, by the
// POST's UTC hour, with a row per span or log record. Nothing deletes files:
// give the bucket a lifecycle rule. The rows are schema v0-unstable:
//
//   traces  service, trace_id, span_id, parent_span_id, name, kind,
//           start_unix_us, duration_us, ok, error, cell, url, http_status,
//           attributes
//   logs    service, trace_id, span_id, time_unix_us, severity_number,
//           severity_text, body, cell, attributes
//
// `attributes` holds every attribute not promoted to a column, the
// resource's included. A log's body is its console line as a terminal shows
// it.

const SIGNALS = { '/v1/traces': spans, '/v1/logs': logs };

export default {
  async fetch(request, env) {
    const rows = SIGNALS[new URL(request.url).pathname];
    if (request.method !== 'POST' || !rows) {
      return new Response(null, { status: 404 });
    }
    const lines = rows(await request.json());
    if (lines.length > 0) {
      const signal = rows === spans ? 'traces' : 'logs';
      const body = new Blob([
        lines.map((l) => JSON.stringify(l) + '\n').join(''),
      ])
        .stream()
        .pipeThrough(new CompressionStream('gzip'));
      const put = await env.STORE.fetch(`http://store/${key(signal)}`, {
        method: 'PUT',
        body: await new Response(body).arrayBuffer(),
      });
      if (!put.ok) return new Response(await put.text(), { status: 502 });
    }
    return Response.json({});
  },
};

function key(signal) {
  const now = new Date();
  const hour = now.toISOString().slice(0, 13).replace(/[-T]/g, '/');
  const rand = crypto.getRandomValues(new Uint32Array(1))[0];
  return `${signal}/${hour}/${now.getTime()}-${rand.toString(16).padStart(8, '0')}.ndjson.gz`;
}

function spans(request) {
  return each(
    request.resourceSpans,
    'scopeSpans',
    'spans',
    (span, service, resource) => {
      const attrs = map(span.attributes);
      const status = span.status ?? {};
      const ok = status.code !== 2;
      const start = us(span.startTimeUnixNano);
      return {
        service,
        trace_id: span.traceId,
        span_id: span.spanId,
        parent_span_id: span.parentSpanId ?? null,
        name: span.name,
        kind: span.kind ?? 0,
        start_unix_us: start,
        duration_us: us(span.endTimeUnixNano) - start,
        ok,
        error: ok ? null : (status.message ?? ''),
        cell: take(attrs, 'cloudflare.durable_object.id'),
        url: take(attrs, 'url.full'),
        http_status: take(attrs, 'http.response.status_code'),
        attributes: { ...resource, ...attrs },
      };
    }
  );
}

function logs(request) {
  return each(
    request.resourceLogs,
    'scopeLogs',
    'logRecords',
    (record, service, resource) => {
      const attrs = map(record.attributes);
      const errors = take(attrs, 'cloudflare.log.error_info') ?? [];
      const n = record.severityNumber ?? 0;
      return {
        service,
        trace_id: record.traceId ?? null,
        span_id: record.spanId ?? null,
        time_unix_us: us(record.timeUnixNano),
        severity_number: n,
        severity_text:
          n >= 17
            ? 'ERROR'
            : n >= 13
              ? 'WARN'
              : n >= 1 && n <= 8
                ? 'DEBUG'
                : 'INFO',
        body: line(record.body, errors),
        cell: take(attrs, 'cloudflare.durable_object.id'),
        attributes: { ...resource, ...attrs },
      };
    }
  );
}

// A row from `row(item, service, resource)` for each item of each scope of each resource.
function each(resources, scopes, items, row) {
  return (resources ?? []).flatMap((r) => {
    const resource = map(r.resource?.attributes);
    const service = take(resource, 'service.name') ?? '';
    return (r[scopes] ?? []).flatMap((s) =>
      (s[items] ?? []).map((i) => row(i, service, resource))
    );
  });
}

// The console line of a log's body, the JSON array of the call's arguments: each argument that
// was an Error as its stack, or `name: message`, the rest as strings or JSON, space-separated.
function line(body, errors) {
  const args = body?.arrayValue
    ? (body.arrayValue.values ?? [])
    : body
      ? [body]
      : [];
  return args
    .map((arg, i) => {
      const e = errors[i];
      if (e && typeof e === 'object')
        return e.stack ?? `${e.name ?? ''}: ${e.message ?? ''}`;
      const v = value(arg);
      return typeof v === 'string' ? v : JSON.stringify(v);
    })
    .join(' ');
}

function map(attributes) {
  return Object.fromEntries(
    (attributes ?? []).map((kv) => [kv.key, value(kv.value)])
  );
}

function value(v) {
  if (v == null) return null;
  if ('stringValue' in v) return v.stringValue;
  if ('boolValue' in v) return v.boolValue;
  if ('intValue' in v) return Number(v.intValue);
  if ('doubleValue' in v) return v.doubleValue;
  if ('arrayValue' in v) return (v.arrayValue.values ?? []).map(value);
  if ('kvlistValue' in v) return map(v.kvlistValue.values);
  return null;
}

function take(attrs, key) {
  const v = attrs[key] ?? null;
  delete attrs[key];
  return v;
}

function us(nanos) {
  return Number(BigInt(nanos ?? 0) / 1000n);
}
