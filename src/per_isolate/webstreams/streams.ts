'use strict';

const {
  ReadableStream,
  ReadableStreamDefaultReader,
  ReadableStreamBYOBReader,
  ReadableStreamDefaultController,
  ReadableByteStreamController,
  ReadableStreamBYOBRequest,
  ReadableStreamDrainingReader,
} = require('webstreams/readable');

const {
  ByteLengthQueuingStrategy,
  CountQueuingStrategy,
} = require('webstreams/strategies');

const {
  WritableStream,
  WritableStreamDefaultWriter,
  WritableStreamDefaultController,
} = require('webstreams/writable');

const {
  TransformStream,
  TransformStreamDefaultController,
} = require('webstreams/transform');

const {
  IdentityTransformStream,
  FixedLengthStream,
} = require('webstreams/identity');

const { TextEncoderStream, TextDecoderStream } = require('webstreams/encoding');

module.exports = {
  ReadableStream,
  ReadableStreamDefaultReader,
  ReadableStreamBYOBReader,
  ReadableStreamDefaultController,
  ReadableByteStreamController,
  ReadableStreamBYOBRequest,
  ByteLengthQueuingStrategy,
  CountQueuingStrategy,
  WritableStream,
  WritableStreamDefaultWriter,
  WritableStreamDefaultController,
  TransformStream,
  TransformStreamDefaultController,
  IdentityTransformStream,
  FixedLengthStream,
  TextEncoderStream,
  TextDecoderStream,
  // Internal-only reader (the C++ bridge's bulk-read surface). Installed on
  // globalThis by main.ts ONLY under the internal-testing
  // expose_draining_reader flag, for exercising expectedLength pass-through
  // and draining reads from JS tests.
  ReadableStreamDrainingReader,
};
