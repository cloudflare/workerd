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

// TEMPORARY (native-stream-integration phase 2): the native-source marker
// symbol, exposed so native-marked underlying sources can be constructed
// from tests before the real JSG plumbing hands capabilities over from the
// C++ side. This module is only reachable through the (equally temporary)
// lazy `globalThis.streams` dev surface installed by main.ts — neither is
// part of the public API, and this export is REMOVED when the phase 3
// C++ handshake lands. Everything else in nativeStreamInternals stays
// module-private.
const { nativeStreamInternals } = require('webstreams/native');

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
  // TEMPORARY — see the comment at the require above.
  kNativeSource: nativeStreamInternals.kNativeSource,
  kExtractNativeSource: nativeStreamInternals.kExtractNativeSource,
  kNativeSink: nativeStreamInternals.kNativeSink,
  kExtractNativeSink: nativeStreamInternals.kExtractNativeSink,
  // TEMPORARY: internal-only reader (the C++ bridge surface), exposed so
  // tests can exercise expectedLength pass-through and draining reads.
  ReadableStreamDrainingReader,
};
