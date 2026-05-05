// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// Regression tests for deserialization hardening. These tests verify that
// crafted V8 structured clone payloads targeting various host object
// deserializers produce graceful DataCloneError exceptions rather than
// crashing the process via KJ_ASSERT / KJ_ASSERT_NONNULL -> abort().
//
// Each test sends a crafted serializedBody via Fetcher::queue() that encodes
// a malformed host object. The host object tag dispatches to the target
// deserializer, which should throw DataCloneError.
//
// V8 wire format reference:
//   0xff 0x0f     - V8 structured clone header (version 15)
//   0x5c          - kHostObject (delegates to ReadHostObject)
//   Then a varint uint32 workerd tag dispatches to the specific deserializer.
//
// Workerd serialization tags (from worker-interface.capnp):
//   4 = HEADERS, 5 = REQUEST, 6 = RESPONSE, 7 = DOM_EXCEPTION (legacy)

import assert from 'node:assert';

// Helper: encode a uint32 as a varint (LEB128)
function varint(n) {
  const bytes = [];
  do {
    let b = n & 0x7f;
    n >>>= 7;
    if (n > 0) b |= 0x80;
    bytes.push(b);
  } while (n > 0);
  return bytes;
}

// V8 wire format header
const V8_HEADER = [0xff, 0x0f];
const HOST_OBJECT = [0x5c];

// V8 value tags
const kOneByteString = 0x22;
const kInt32 = 0x49;
const kBeginJSObject = 0x6f;
const kEndJSObject = 0x7b;

// Workerd serialization tags
const TAG_HEADERS = 4;
const TAG_REQUEST = 5;
const TAG_RESPONSE = 6;
const TAG_DOM_EXCEPTION_LEGACY = 7;

function makePayload(...parts) {
  return new Uint8Array(parts.flat()).buffer;
}

// Encode a one-byte string in V8 wire format: kOneByteString + varint(len) + bytes
function v8String(s) {
  const bytes = Array.from(s, (c) => c.charCodeAt(0));
  return [kOneByteString, ...varint(bytes.length), ...bytes];
}

// Encode a V8 integer: kInt32 + zigzag(0) = 0
const v8Int0 = [kInt32, 0x00];

// Helper to send a crafted payload and assert the process survives.
async function sendAndSurvive(env, id, payload) {
  const result = await env.SERVICE.queue('hardening-test', [
    {
      id,
      timestamp: new Date(),
      serializedBody: payload,
      attempts: 1,
    },
  ]);
  assert.strictEqual(result.outcome, 'exception');
}

// =========================================================================
// Headers tests (tag 4)
// =========================================================================

// Headers deserializer wire format:
//   varint(guard) + varint(count) + [varint(commonId) + ...]...
// The count bounds check rejects count > 1024.
export const headersCountOverflow = {
  async test(ctrl, env) {
    const payload = makePayload(
      V8_HEADER,
      HOST_OBJECT,
      varint(TAG_HEADERS),
      varint(0), // guard = IMMUTABLE
      varint(1025) // count = 1025 (exceeds maximum of 1024)
    );
    await sendAndSurvive(env, 'headers-count-overflow', payload);
  },
};

// =========================================================================
// DOMException legacy tests (tag 7)
// =========================================================================

// DOMException legacy wire format:
//   varint64(nameLen) + nameBytes + readValue(js) [expecting object]
// The object must have string "message" and "stack" properties.

// Test: readValue returns a non-object (integer instead of object)
export const domExceptionNonObject = {
  async test(ctrl, env) {
    const payload = makePayload(
      V8_HEADER,
      HOST_OBJECT,
      varint(TAG_DOM_EXCEPTION_LEGACY),
      // readLengthDelimitedString for name: varint64(4) + "Test"
      varint(4),
      [0x54, 0x65, 0x73, 0x74],
      // readValue: an integer, not an object
      v8Int0
    );
    await sendAndSurvive(env, 'domexception-non-object', payload);
  },
};

// Test: object has non-string "message" property
export const domExceptionNonStringMessage = {
  async test(ctrl, env) {
    const payload = makePayload(
      V8_HEADER,
      HOST_OBJECT,
      varint(TAG_DOM_EXCEPTION_LEGACY),
      // name
      varint(4),
      [0x54, 0x65, 0x73, 0x74],
      // readValue: a JS object with integer "message" and string "stack"
      [kBeginJSObject],
      v8String('message'),
      v8Int0, // message: 0 (not a string)
      v8String('stack'),
      v8String(''), // stack: "" (valid string)
      [kEndJSObject, ...varint(2)]
    );
    await sendAndSurvive(env, 'domexception-non-string-message', payload);
  },
};

// Test: object has non-string "stack" property
export const domExceptionNonStringStack = {
  async test(ctrl, env) {
    const payload = makePayload(
      V8_HEADER,
      HOST_OBJECT,
      varint(TAG_DOM_EXCEPTION_LEGACY),
      // name
      varint(4),
      [0x54, 0x65, 0x73, 0x74],
      // readValue: a JS object with string "message" and integer "stack"
      [kBeginJSObject],
      v8String('message'),
      v8String(''), // message: "" (valid)
      v8String('stack'),
      v8Int0, // stack: 0 (not a string)
      [kEndJSObject, ...varint(2)]
    );
    await sendAndSurvive(env, 'domexception-non-string-stack', payload);
  },
};

// =========================================================================
// Request tests (tag 5)
// =========================================================================

// Request deserializer wire format:
//   readLengthDelimitedString [URL] + readValue [init dict]
// If initDictHandler.tryUnwrap fails, throws DataCloneError.
export const requestInvalidInit = {
  async test(ctrl, env) {
    const payload = makePayload(
      V8_HEADER,
      HOST_OBJECT,
      varint(TAG_REQUEST),
      // readLengthDelimitedString for URL: varint64(1) + "/"
      varint(1),
      [0x2f],
      // readValue: an integer instead of a RequestInitializerDict
      v8Int0
    );
    await sendAndSurvive(env, 'request-invalid-init', payload);
  },
};

// =========================================================================
// Response tests (tag 6)
// =========================================================================

// Response deserializer wire format:
//   readValue [body] + readValue [init dict]
// If streamHandler.tryUnwrap fails on body, throws DataCloneError.
export const responseInvalidBody = {
  async test(ctrl, env) {
    const payload = makePayload(
      V8_HEADER,
      HOST_OBJECT,
      varint(TAG_RESPONSE),
      // readValue for body: an integer instead of a ReadableStream
      v8Int0
    );
    await sendAndSurvive(env, 'response-invalid-body', payload);
  },
};

export default {
  async queue(batch, env, ctx) {
    batch.ackAll();
  },
};
