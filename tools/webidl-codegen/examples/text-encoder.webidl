// Simple test case: TextEncoder API
// https://encoding.spec.whatwg.org/#interface-textencoder

[Exposed=*]
interface TextEncoder {
  constructor();

  [SameObject] readonly attribute DOMString encoding;

  Uint8Array encode(optional DOMString input = "");

  // Union type example - accepts string or BufferSource
  Uint8Array encodeInto((DOMString or BufferSource) source);
};
