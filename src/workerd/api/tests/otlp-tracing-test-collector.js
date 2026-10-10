// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// A stand-in for an OpenTelemetry collector. POST /v1/traces takes an OTLP/HTTP protobuf
// ExportTraceServiceRequest; GET /requests returns every request received so far as JSON, with
// the POST's content type beside the decoded resource and spans.

// Yields [field number, value] for each field of an encoded protobuf message. A varint is a
// BigInt, a 32-bit value a number, and a 64-bit or length-delimited value the bytes themselves.
function* fields(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let pos = 0;
  const varint = () => {
    let result = 0n;
    for (let shift = 0n; ; shift += 7n) {
      const byte = bytes[pos++];
      result |= BigInt(byte & 0x7f) << shift;
      if (!(byte & 0x80)) return result;
    }
  };
  while (pos < bytes.length) {
    const tag = Number(varint());
    const number = tag >> 3;
    switch (tag & 7) {
      case 0:
        yield [number, varint()];
        break;
      case 1:
        yield [number, bytes.subarray(pos, pos + 8)];
        pos += 8;
        break;
      case 2: {
        const length = Number(varint());
        yield [number, bytes.subarray(pos, pos + length)];
        pos += length;
        break;
      }
      case 5:
        yield [number, view.getUint32(pos, true)];
        pos += 4;
        break;
      default:
        throw new Error(`unexpected wire type ${tag & 7}`);
    }
  }
}

const text = (bytes) => new TextDecoder().decode(bytes);
const hex = (bytes) =>
  Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
const fixed64 = (bytes) =>
  new DataView(bytes.buffer, bytes.byteOffset, 8).getBigUint64(0, true);

// opentelemetry.proto.common.v1.AnyValue
function anyValue(bytes) {
  for (const [number, value] of fields(bytes)) {
    switch (number) {
      case 1:
        return text(value);
      case 2:
        return value !== 0n;
      case 3:
        return Number(BigInt.asIntN(64, value));
      case 4:
        return new DataView(value.buffer, value.byteOffset, 8).getFloat64(
          0,
          true
        );
    }
  }
  throw new Error('attribute without a value');
}

// A repeated opentelemetry.proto.common.v1.KeyValue field, collected into `attributes`.
function addAttribute(attributes, bytes) {
  let key;
  let value;
  for (const [number, field] of fields(bytes)) {
    if (number === 1) key = text(field);
    if (number === 2) value = anyValue(field);
  }
  attributes[key] = value;
}

// opentelemetry.proto.trace.v1.Span.Event
function decodeEvent(bytes) {
  const event = { attributes: {} };
  for (const [number, value] of fields(bytes)) {
    if (number === 1) event.timeUnixNano = Number(fixed64(value));
    if (number === 2) event.name = text(value);
    if (number === 3) addAttribute(event.attributes, value);
  }
  return event;
}

// opentelemetry.proto.trace.v1.Status
function decodeStatus(bytes) {
  const status = { code: 0, message: '' };
  for (const [number, value] of fields(bytes)) {
    if (number === 2) status.message = text(value);
    if (number === 3) status.code = Number(value);
  }
  return status;
}

// opentelemetry.proto.trace.v1.Span
function decodeSpan(bytes) {
  const span = { attributes: {}, events: [] };
  for (const [number, value] of fields(bytes)) {
    switch (number) {
      case 1:
        span.traceId = hex(value);
        break;
      case 2:
        span.spanId = hex(value);
        break;
      case 4:
        span.parentSpanId = hex(value);
        break;
      case 5:
        span.name = text(value);
        break;
      case 6:
        span.kind = Number(value);
        break;
      case 7:
        span.startTimeUnixNano = Number(fixed64(value));
        break;
      case 8:
        span.endTimeUnixNano = Number(fixed64(value));
        break;
      case 9:
        addAttribute(span.attributes, value);
        break;
      case 11:
        span.events.push(decodeEvent(value));
        break;
      case 15:
        span.status = decodeStatus(value);
        break;
      case 16:
        span.flags = value;
        break;
    }
  }
  return span;
}

// opentelemetry.proto.collector.trace.v1.ExportTraceServiceRequest, which the runtime fills with
// one ResourceSpans holding one ScopeSpans.
function decodeRequest(bytes) {
  const request = { resource: {}, spans: [] };
  for (const [, resourceSpans] of fields(bytes)) {
    for (const [number, value] of fields(resourceSpans)) {
      if (number === 1) {
        for (const [, attribute] of fields(value)) {
          addAttribute(request.resource, attribute);
        }
      }
      if (number === 2) {
        for (const [, span] of fields(value)) {
          request.spans.push(decodeSpan(span));
        }
      }
    }
  }
  return request;
}

const requests = [];

export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (request.method === 'POST' && url.pathname === '/v1/traces') {
      requests.push({
        contentType: request.headers.get('content-type'),
        ...decodeRequest(new Uint8Array(await request.arrayBuffer())),
      });
      return new Response(null, { status: 200 });
    }
    if (url.pathname === '/requests') {
      return Response.json(requests);
    }
    return new Response('not found', { status: 404 });
  },
};
