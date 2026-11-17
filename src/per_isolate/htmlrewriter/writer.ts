'use strict';

// The writer methods of the streams implementation main.ts installed, which
// the identity pipe's ends belong to. Not primordials, which are captured
// before main.ts installs the TypeScript streams. main.ts loads this module
// as it installs HTMLRewriter, before any user code runs, so that the
// captures hold however late htmlrewriter.ts is loaded.

const { uncurryThis } = primordials;

type Writer = WritableStreamDefaultWriter<Uint8Array>;

const WritableStreamGetWriter = uncurryThis(
  globalThis.WritableStream.prototype.getWriter
) as (stream: WritableStream<Uint8Array>) => Writer;
const WriterWrite = uncurryThis(
  globalThis.WritableStreamDefaultWriter.prototype.write
) as (writer: Writer, chunk: Uint8Array) => Promise<void>;
const WriterClose = uncurryThis(
  globalThis.WritableStreamDefaultWriter.prototype.close
) as (writer: Writer) => Promise<void>;
const WriterAbort = uncurryThis(
  globalThis.WritableStreamDefaultWriter.prototype.abort
) as (writer: Writer, reason?: unknown) => Promise<void>;

export type HtmlRewriterWriter = {
  WritableStreamGetWriter: typeof WritableStreamGetWriter;
  WriterWrite: typeof WriterWrite;
  WriterClose: typeof WriterClose;
  WriterAbort: typeof WriterAbort;
};

module.exports = {
  WritableStreamGetWriter,
  WriterWrite,
  WriterClose,
  WriterAbort,
};
