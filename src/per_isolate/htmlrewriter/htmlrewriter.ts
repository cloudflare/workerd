'use strict';

// HTMLRewriter, under the TS_HTML_REWRITER autogate. It replaces the C++
// class in api/html-rewriter.h, over the same parser, lol-html, which the
// Rust crate in api/html-rewriter/ binds and the embedder provides as
// utils.htmlRewriter (Worker::Api::newHtmlRewriterUtils()).
// src/tests/streams/htmlrewriter/AGENTS.md records where each difference
// below is pinned.
//
// DIFFERENCES FROM THE C++ IMPLEMENTATION
//
// - The output is written to the transformed body at the end of each
//   chunk, and before each handler that returns a promise, as the handler
//   starts; the rewriter waits for it to be read only at the end of the
//   chunk. The C++ one writes the output before each handler after the
//   handler, and waits for it to be read before going on, so that a
//   canceled transformed body stops the rewrite at that handler; here, at
//   the next handler that returns a promise after the failed write, or at
//   the end of the chunk.
// - The token classes' properties (Element's tagName, attributes, removed
//   and namespaceURI, and so on) are own accessors of each token, rather
//   than the own data properties JSG makes for an instance property. They
//   read the same, enumerate the same, and serialize the same with
//   JSON.stringify(); assigning to a read-only one in strict mode throws
//   the language's TypeError for a property with only a getter.
// - Calling a constructor without `new` throws the language's TypeError
//   rather than JSG's. The token classes' constructors, reachable as
//   `token.constructor`, throw a TypeError when called.
// - The messages of the TypeErrors for arguments of the wrong type follow
//   JSG's but are not all the same.
// - This module is loaded on the first read of the HTMLRewriter global,
//   which until then, or until it is assigned, is an accessor rather than
//   the data property JSG makes for a nested type (see main.ts).
//
// HOW IT FITS TOGETHER
//
// transform() does what the C++ one does, with the same native parts: it
// creates a native rewriter for the handlers registered so far and a native
// identity pipe, whose readable end is the transformed body, and pumps the
// original body with the native pump (utils.pumpTo()) to a sink that writes
// each chunk to the rewriter and its output to the pipe, completing once the
// pipe has taken it. So the original body is read, canceled and errored as
// with the C++ one, errors pass through KJ exceptions in the same places,
// and canceling the transformed body fails the next write to the pipe,
// which the pump turns into canceling the original body.
//
// The native rewriter calls one function, the dispatcher below, for every
// handler lol-html runs, with a native token handle that it invalidates
// once the handler (and the promise it returns, if any) is done; the
// dispatcher wraps the handle in a fresh token object and calls the user's
// handler. The output lol-html produces comes back from each write, but
// for the output before a handler that returns a promise, which the
// dispatcher takes and writes as the handler starts.
//
// Content from a ReadableStream or a Response is numbered and passed to the
// token's *Stream() method; lol-html dispatches the number when it writes
// the content, with a sink that the native pump pumps the stream to. lol-html drops content
// that it never writes without saying so, so the streams are kept until the
// rewrite ends.

import type { RuntimeClassHelpers } from '../runtime-class';
import type { WebIdlHelpers } from '../webidl';
import type { HtmlRewriterWriter } from './writer';

const {
  Error,
  FunctionPrototypeCall,
  HeadersGet,
  IteratorPrototype,
  ObjectDefineProperties,
  ObjectDefineProperty,
  ObjectFreeze,
  ObjectGetOwnPropertyDescriptor,
  ObjectGetPrototypeOf,
  ObjectSetPrototypeOf,
  PromisePrototypeThen,
  PromiseReject,
  PromiseResolve,
  ResponseBodyGet,
  ResponseCtor,
  ResponseHeadersGet,
  ResponsePrototype,
  ResponseTypeGet,
  Symbol,
  SymbolIterator,
  SymbolToStringTag,
  TypeError,
  TypedArrayPrototypeGetByteLength,
  Uint8Array,
} = primordials;

// The prototype of internal records. A literal with `__proto__: null` makes
// a dictionary-mode object, which is slower to read.
const { kNullProtoObject } = require('webstreams/common');
const { setNameAndLength } = require('runtime-class') as RuntimeClassHelpers;
const { isObject } = require('webidl') as WebIdlHelpers;

const {
  isPromise,
  markPromiseHandled,
  mimeTypeParameter,
  newIdentityPipe,
  pumpTo,
  readableStreamState,
} = utils;

const maybeNative = utils.htmlRewriter;
if (maybeNative === undefined) {
  throw new TypeError(
    'The ts-html-rewriter autogate is enabled, but this runtime does not ' +
      'provide the native HTMLRewriter.'
  );
}
const native: HtmlRewriterUtils = maybeNative;

type Writer = WritableStreamDefaultWriter<Uint8Array>;
const { WritableStreamGetWriter, WriterWrite, WriterClose, WriterAbort } =
  require('htmlrewriter/writer') as HtmlRewriterWriter;

// JSG's messages, so that misuse reads the same as with the C++ classes.
const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';
const kIllegalConstructor = 'Illegal constructor';
const kStringContentOnly =
  'Replacing content in HTML comments using a ReadableStream or Response ' +
  'object is not implemented. You must provide a string.';
const kAttributesModified =
  'The attributes of this element have been modified during iteration. ' +
  'You must create a new iterator after modifying attributes.';

// The handler kinds the native rewriter dispatches (HandlerKind in
// api/html-rewriter/rewriter.rs).
const kElement = 0;
const kComments = 1;
const kText = 2;
const kDoctype = 3;
const kDocumentComments = 4;
const kDocumentText = 5;
const kDocumentEnd = 6;
const kEndTag = 7;
const kStream = 8;

// Lets the module construct the token classes, whose constructors are
// otherwise illegal.
const kConstruct: unique symbol = Symbol('kConstruct') as never;

type Handler = (this: unknown, token: unknown) => unknown;

type ElementHandlerSet = {
  readonly selector: HtmlSelector;
  readonly object: object;
  readonly element: Handler | undefined;
  readonly comments: Handler | undefined;
  readonly text: Handler | undefined;
};

type DocumentHandlerSet = {
  readonly object: object;
  readonly doctype: Handler | undefined;
  readonly comments: Handler | undefined;
  readonly text: Handler | undefined;
  readonly end: Handler | undefined;
};

function argumentError(
  method: string,
  type: string,
  index: number,
  expected: string
): TypeError {
  return new TypeError(
    `Failed to execute '${method}' on '${type}': parameter ${index} is not ` +
      `of type '${expected}'.`
  );
}

// Whether `value` inherits from Response.prototype; its getters then check
// that it is one.
function isResponse(value: unknown): value is Response {
  if (!isObject(value)) return false;
  for (let proto = ObjectGetPrototypeOf(value); proto !== null;) {
    if (proto === ResponsePrototype) return true;
    proto = ObjectGetPrototypeOf(proto);
  }
  return false;
}

// A handlers dictionary, as JSG converts one: undefined and null are empty,
// any other non-object fails, and each member must be a function if given.
function readHandlers<K extends string>(
  value: unknown,
  method: string,
  dictionary: string,
  names: readonly K[]
): { object: object; handlers: Record<K, Handler | undefined> } {
  const handlers = { __proto__: kNullProtoObject } as unknown as Record<
    K,
    Handler | undefined
  >;
  if (value === undefined || value === null) {
    for (let i = 0; i < names.length; i++) {
      handlers[names[i] as K] = undefined;
    }
    return { object: {}, handlers };
  }
  if (!isObject(value)) {
    throw argumentError(
      method,
      'HTMLRewriter',
      method === 'on' ? 2 : 1,
      dictionary
    );
  }
  for (let i = 0; i < names.length; i++) {
    const name = names[i] as K;
    const handler = (value as Record<string, unknown>)[name];
    if (handler !== undefined && typeof handler !== 'function') {
      throw new TypeError(
        `Incorrect type for the '${name}' field on '${dictionary}': the ` +
          `provided value is not of type 'function'.`
      );
    }
    handlers[name] = handler as Handler | undefined;
  }
  return { object: value, handlers };
}

// ContentOptions, as JSG converts it: undefined and null are the defaults.
function readHtml(options: unknown, method: string, type: string): boolean {
  if (options === undefined || options === null) return false;
  if (!isObject(options)) {
    throw argumentError(method, type, 2, 'ContentOptions');
  }
  return !!(options as { html?: unknown }).html;
}

// Content, as JSG converts it: a ReadableStream or a Response (of which only
// the body matters) as itself, and anything else as a string.
const kNoContent: unique symbol = Symbol('kNoContent') as never;
function readContent(
  content: unknown
): string | ReadableStream | typeof kNoContent {
  if (typeof content === 'string') return content;
  if (readableStreamState(content) !== undefined) {
    return content as ReadableStream;
  }
  if (isResponse(content)) return ResponseBodyGet(content) ?? kNoContent;
  return `${content as string}`;
}

function readStringContent(content: unknown): string {
  const value = readContent(content);
  if (typeof value !== 'string') throw new TypeError(kStringContentOnly);
  return value;
}

// What the native rewriter awaits for a handler's result: a promise as
// itself, any other object as JSG's conversion to a promise adopts it, and
// nothing for the rest, which lets the rewriter go on without a turn of the
// event loop.
function awaitable(result: unknown): Promise<unknown> | undefined {
  if (isPromise(result)) return result;
  if (isObject(result)) return PromiseResolve(result);
  return undefined;
}

// Pumps `stream` to `sink`, the content lol-html is writing, as the C++
// ReplacerStreamSink is pumped to.
function pump(stream: ReadableStream, sink: HtmlSinkHandle): Promise<void> {
  return pumpTo(stream, {
    __proto__: kNullProtoObject,
    write: (chunk: ArrayBuffer): undefined => {
      sink.write(new Uint8Array(chunk));
      return undefined;
    },
    end: (): undefined => undefined,
    abort: (): void => {},
  } as PumpSink);
}

// ---------------------------------------------------------------------------
// One transform(): the native rewriter, and what its dispatches need.

class Rewrite {
  readonly handle: HtmlRewriterHandle;
  readonly #elements: readonly ElementHandlerSet[];
  readonly #documents: readonly DocumentHandlerSet[];
  // The handlers onEndTag() registered, by number, until they run.
  readonly #endTags: (Handler | undefined)[] = [];
  // The content streams, by number, until the rewrite ends.
  #streams: (ReadableStream | undefined)[] = [];
  #writer: Writer | undefined = undefined;
  // The last write to the pipe; the rewriter's write or end completes once
  // it has.
  #lastWrite: Promise<void> = PromiseResolve();
  // Whether a write to the pipe failed, and what with; the rewriter stops
  // at its next handler.
  #writeFailed = false;
  #writeError: unknown = undefined;
  // What the rewrite failed with, once it has.
  #failed = false;
  #error: unknown = undefined;

  constructor(
    elements: readonly ElementHandlerSet[],
    documents: readonly DocumentHandlerSet[],
    encoding: string
  ) {
    this.#elements = elements;
    this.#documents = documents;
    const elementFlags: NativeElementHandlers[] = [];
    for (let i = 0; i < elements.length; i++) {
      const set = elements[i] as ElementHandlerSet;
      elementFlags[i] = {
        __proto__: kNullProtoObject,
        selector: set.selector,
        element: set.element !== undefined,
        comments: set.comments !== undefined,
        text: set.text !== undefined,
      } as NativeElementHandlers;
    }
    const documentFlags: NativeDocumentHandlers[] = [];
    for (let i = 0; i < documents.length; i++) {
      const set = documents[i] as DocumentHandlerSet;
      documentFlags[i] = {
        __proto__: kNullProtoObject,
        doctype: set.doctype !== undefined,
        comments: set.comments !== undefined,
        text: set.text !== undefined,
        end: set.end !== undefined,
      } as NativeDocumentHandlers;
    }
    this.handle = native.newRewriter(
      elementFlags,
      documentFlags,
      encoding,
      (kind, id, token) => this.#dispatch(kind, id, token)
    );
  }

  addEndTagHandler(handler: Handler): number {
    const id = this.#endTags.length;
    this.#endTags[id] = handler;
    return id;
  }

  addStream(stream: ReadableStream): number {
    const id = this.#streams.length;
    this.#streams[id] = stream;
    return id;
  }

  // Starts pumping `body` to the rewriter, and returns the transformed body.
  start(body: ReadableStream): ReadableStream<Uint8Array> {
    const [readable, writable] = newIdentityPipe();
    this.#writer = WritableStreamGetWriter(writable);
    markPromiseHandled(
      pumpTo(body, {
        __proto__: kNullProtoObject,
        write: (chunk: ArrayBuffer): Promise<void> =>
          this.#run(
            this.#call(() => this.handle.writeAsync(new Uint8Array(chunk)))
          ),
        end: (): Promise<void> =>
          PromisePrototypeThen(
            this.#run(this.#call(() => this.handle.endAsync())),
            () => {
              this.#streams = [];
              return WriterClose(this.#writer as Writer);
            }
          ),
        abort: (reason: unknown): void => this.#fail(reason),
      } as PumpSink)
    );
    return readable;
  }

  // The rewriter's write or end, as a promise however it fails.
  #call(call: () => Promise<Uint8Array>): Promise<Uint8Array> {
    if (this.#failed) return PromiseReject(this.#error);
    try {
      return call();
    } catch (error) {
      return PromiseReject(error);
    }
  }

  // Writes the output of the rewriter's write or end to the pipe, and
  // resolves once the pipe has taken it, as Rewriter::finishWrite() does.
  // A failure fails the rewrite: the transformed body is aborted with it,
  // and the pump cancels the original body with it.
  #run(output: Promise<Uint8Array>): Promise<void> {
    return PromisePrototypeThen(
      PromisePrototypeThen(output, (bytes: Uint8Array) => {
        this.#write(bytes);
        return this.#lastWrite;
      }),
      undefined,
      (error: unknown) => {
        const reason = this.#writeFailed ? this.#writeError : error;
        this.#fail(reason);
        throw reason;
      }
    );
  }

  #write(bytes: Uint8Array | undefined): void {
    if (bytes === undefined || TypedArrayPrototypeGetByteLength(bytes) === 0) {
      return;
    }
    const write = WriterWrite(this.#writer as Writer, bytes);
    this.#lastWrite = write;
    PromisePrototypeThen(write, undefined, (error: unknown) => {
      if (this.#writeFailed) return;
      this.#writeFailed = true;
      this.#writeError = error;
    });
  }

  #fail(error: unknown): void {
    if (this.#failed) return;
    this.#failed = true;
    this.#error = error;
    this.#streams = [];
    try {
      this.handle.abort();
    } catch {
      // A write is running; it fails at its next dispatch.
    }
    markPromiseHandled(WriterAbort(this.#writer as Writer, error));
  }

  #dispatch(
    kind: number,
    id: number,
    token: unknown
  ): Promise<unknown> | undefined {
    if (this.#writeFailed) throw this.#writeError;
    const result = this.#runHandler(kind, id, token);
    // The output before a handler that waits is written while it waits.
    // Otherwise the output stays in the rewriter until the end of the chunk,
    // since a write per handler costs more than most handlers do.
    if (result !== undefined) this.#write(this.handle.takeOutput());
    return result;
  }

  #runHandler(
    kind: number,
    id: number,
    token: unknown
  ): Promise<unknown> | undefined {
    switch (kind) {
      case kElement: {
        const set = this.#elements[id] as ElementHandlerSet;
        return call(set.element, set.object, newElement(token, this));
      }
      case kComments: {
        const set = this.#elements[id] as ElementHandlerSet;
        return call(set.comments, set.object, newComment(token));
      }
      case kText: {
        const set = this.#elements[id] as ElementHandlerSet;
        return call(set.text, set.object, newText(token, this));
      }
      case kDoctype: {
        const set = this.#documents[id] as DocumentHandlerSet;
        return call(set.doctype, set.object, newDoctype(token));
      }
      case kDocumentComments: {
        const set = this.#documents[id] as DocumentHandlerSet;
        return call(set.comments, set.object, newComment(token));
      }
      case kDocumentText: {
        const set = this.#documents[id] as DocumentHandlerSet;
        return call(set.text, set.object, newText(token, this));
      }
      case kDocumentEnd: {
        const set = this.#documents[id] as DocumentHandlerSet;
        return call(set.end, set.object, newDocumentEnd(token));
      }
      case kEndTag: {
        const handler = this.#endTags[id];
        this.#endTags[id] = undefined;
        return call(handler, undefined, newEndTag(token, this));
      }
      case kStream: {
        const stream = this.#streams[id] as ReadableStream;
        this.#streams[id] = undefined;
        return pump(stream, token as HtmlSinkHandle);
      }
    }
    return undefined;
  }
}

function call(
  handler: Handler | undefined,
  thisArg: unknown,
  token: unknown
): Promise<unknown> | undefined {
  return awaitable(
    FunctionPrototypeCall(handler as Handler, thisArg, token) as unknown
  );
}

// ---------------------------------------------------------------------------
// HTMLRewriter.

let requireRewriter: (receiver: unknown) => void;

class HTMLRewriter {
  #elements: ElementHandlerSet[] = [];
  #documents: DocumentHandlerSet[] = [];

  static {
    requireRewriter = (receiver): void => {
      if (!isObject(receiver) || !(#elements in receiver)) {
        throw new TypeError(kIllegalInvocation);
      }
    };
  }

  on(selector: unknown, handlers: unknown): this {
    requireRewriter(this);
    const selectorString = `${selector as string}`;
    const { object, handlers: read } = readHandlers(
      handlers,
      'on',
      'ElementContentHandlers',
      ['element', 'comments', 'text'] as const
    );
    const parsed = native.parseSelector(selectorString);
    this.#elements[this.#elements.length] = {
      __proto__: kNullProtoObject,
      selector: parsed,
      object,
      element: read.element,
      comments: read.comments,
      text: read.text,
    } as ElementHandlerSet;
    return this;
  }

  onDocument(handlers: unknown): this {
    requireRewriter(this);
    const { object, handlers: read } = readHandlers(
      handlers,
      'onDocument',
      'DocumentContentHandlers',
      ['doctype', 'comments', 'text', 'end'] as const
    );
    this.#documents[this.#documents.length] = {
      __proto__: kNullProtoObject,
      object,
      doctype: read.doctype,
      comments: read.comments,
      text: read.text,
      end: read.end,
    } as DocumentHandlerSet;
    return this;
  }

  transform(response: unknown): Response {
    requireRewriter(this);
    if (!isResponse(response)) {
      throw argumentError('transform', 'HTMLRewriter', 1, 'Response');
    }
    if (ResponseTypeGet(response) === 'error') {
      throw new TypeError('HTMLRewriter cannot transform an error response');
    }
    const body = ResponseBodyGet(response);
    if (body === null) return response;

    let encoding = 'utf-8';
    const contentType = HeadersGet(
      ResponseHeadersGet(response),
      'content-type'
    );
    if (contentType !== null) {
      const charset = mimeTypeParameter(contentType, 'charset');
      if (charset !== undefined) encoding = charset;
    }

    // Copies of the handlers, so that later on() calls don't change this
    // rewrite. Creating the rewriter checks the encoding, before the body is
    // disturbed.
    const elements: ElementHandlerSet[] = [];
    for (let i = 0; i < this.#elements.length; i++) {
      elements[i] = this.#elements[i] as ElementHandlerSet;
    }
    const documents: DocumentHandlerSet[] = [];
    for (let i = 0; i < this.#documents.length; i++) {
      documents[i] = this.#documents[i] as DocumentHandlerSet;
    }
    const rewrite = new Rewrite(elements, documents, encoding);
    return new ResponseCtor(rewrite.start(body), response);
  }
}

// ---------------------------------------------------------------------------
// The tokens. Each wraps a native handle that is valid while its handler
// runs, and throws once it is done.

let elementHandle: (receiver: unknown) => HtmlElementHandle;
let elementRewrite: (receiver: unknown) => Rewrite;
let elementAttributesEpoch: (receiver: unknown) => { epoch: number };

class Element {
  #handle: HtmlElementHandle;
  #rewrite: Rewrite;
  // Bumped when the attributes change, which ends the iterators over them.
  #attributes = { epoch: 0 };

  static {
    elementHandle = (receiver): HtmlElementHandle => {
      if (!isObject(receiver) || !(#handle in receiver)) {
        throw new TypeError(kIllegalInvocation);
      }
      return receiver.#handle;
    };
    elementRewrite = (receiver): Rewrite => (receiver as Element).#rewrite;
    elementAttributesEpoch = (receiver): { epoch: number } =>
      (receiver as Element).#attributes;
  }

  constructor(key: unknown, handle: HtmlElementHandle, rewrite: Rewrite) {
    if (key !== kConstruct) throw new TypeError(kIllegalConstructor);
    this.#handle = handle;
    this.#rewrite = rewrite;
    ObjectDefineProperties(this, kElementProperties);
  }

  getAttribute(name: unknown): string | null {
    return elementHandle(this).getAttribute(`${name as string}`) ?? null;
  }

  hasAttribute(name: unknown): boolean {
    return elementHandle(this).hasAttribute(`${name as string}`);
  }

  setAttribute(name: unknown, value: unknown): this {
    const handle = elementHandle(this);
    handle.setAttribute(`${name as string}`, `${value as string}`);
    this.#attributes.epoch++;
    return this;
  }

  removeAttribute(name: unknown): this {
    const handle = elementHandle(this);
    handle.removeAttribute(`${name as string}`);
    this.#attributes.epoch++;
    return this;
  }

  before(content: unknown, options?: unknown): this {
    rewriteElement(this, 'before', content, options);
    return this;
  }

  after(content: unknown, options?: unknown): this {
    rewriteElement(this, 'after', content, options);
    return this;
  }

  prepend(content: unknown, options?: unknown): this {
    rewriteElement(this, 'prepend', content, options);
    return this;
  }

  append(content: unknown, options?: unknown): this {
    rewriteElement(this, 'append', content, options);
    return this;
  }

  replace(content: unknown, options?: unknown): this {
    rewriteElement(this, 'replace', content, options);
    return this;
  }

  remove(): this {
    elementHandle(this).remove();
    return this;
  }

  removeAndKeepContent(): this {
    elementHandle(this).removeAndKeepContent();
    return this;
  }

  setInnerContent(content: unknown, options?: unknown): this {
    rewriteElement(this, 'setInnerContent', content, options);
    return this;
  }

  onEndTag(handler: unknown): void {
    const handle = elementHandle(this);
    if (typeof handler !== 'function') {
      throw argumentError('onEndTag', 'Element', 1, 'function');
    }
    handle.onEndTag(this.#rewrite.addEndTagHandler(handler as Handler));
  }
}

type ContentMethod =
  'before' | 'after' | 'prepend' | 'append' | 'replace' | 'setInnerContent';

// Inserts `content` with the string or stream variant of the handle's
// `method`. A Response without a body inserts nothing.
function rewriteContent(
  handle: HtmlContentHandle,
  rewrite: Rewrite,
  method: ContentMethod,
  type: string,
  content: unknown,
  options: unknown
): void {
  const value = readContent(content);
  const html = readHtml(options, method, type);
  if (typeof value === 'string') {
    (handle[method] as (content: string, html: boolean) => void)(value, html);
  } else if (value !== kNoContent) {
    const streamMethod = `${method}Stream` as const;
    (handle[streamMethod] as (id: number, html: boolean) => void)(
      rewrite.addStream(value),
      html
    );
  }
}

function rewriteElement(
  element: Element,
  method: ContentMethod,
  content: unknown,
  options: unknown
): void {
  const handle = elementHandle(element);
  rewriteContent(
    handle,
    elementRewrite(element),
    method,
    'Element',
    content,
    options
  );
}

let iteratorState: (receiver: unknown) => AttributesIterator;

class AttributesIterator {
  #handle: HtmlElementHandle;
  #attributes: { epoch: number };
  #epoch: number;
  #index = 0;
  #done = false;

  static {
    iteratorState = (receiver): AttributesIterator => {
      if (!isObject(receiver) || !(#handle in receiver)) {
        throw new TypeError(kIllegalInvocation);
      }
      return receiver;
    };
  }

  constructor(
    key: unknown,
    handle: HtmlElementHandle,
    attributes: { epoch: number }
  ) {
    if (key !== kConstruct) throw new TypeError(kIllegalConstructor);
    this.#handle = handle;
    this.#attributes = attributes;
    this.#epoch = attributes.epoch;
  }

  next(): IteratorResult<[string, string], undefined> {
    const self = iteratorState(this);
    // Throws once the element's handler is done, before anything else.
    self.#handle.attributeCount();
    if (self.#epoch !== self.#attributes.epoch) {
      throw new Error(kAttributesModified);
    }
    if (!self.#done) {
      const attribute = self.#handle.attribute(self.#index);
      if (attribute !== undefined) {
        self.#index++;
        return { done: false, value: attribute };
      }
      self.#done = true;
    }
    return { done: true, value: undefined };
  }

  [SymbolIterator](): this {
    return iteratorState(this) as this;
  }
}

// %IteratorPrototype%, as the C++ one inherits through JSG_INHERIT_INTRINSIC.
ObjectSetPrototypeOf(AttributesIterator.prototype, IteratorPrototype);

const kElementProperties = ObjectFreeze({
  __proto__: kNullProtoObject,
  tagName: accessor(
    function (this: unknown): string {
      return elementHandle(this).tagName();
    },
    function (this: unknown, name: unknown): void {
      elementHandle(this).setTagName(`${name as string}`);
    }
  ),
  attributes: accessor(function (this: unknown): AttributesIterator {
    return new AttributesIterator(
      kConstruct,
      elementHandle(this),
      elementAttributesEpoch(this)
    );
  }),
  removed: accessor(function (this: unknown): boolean {
    return elementHandle(this).removed();
  }),
  namespaceURI: accessor(function (this: unknown): string {
    return elementHandle(this).namespaceUri();
  }),
}) as PropertyDescriptorMap;

let endTagHandle: (receiver: unknown) => HtmlEndTagHandle;
let endTagRewrite: (receiver: unknown) => Rewrite;

class EndTag {
  #handle: HtmlEndTagHandle;
  #rewrite: Rewrite;

  static {
    endTagHandle = (receiver): HtmlEndTagHandle => {
      if (!isObject(receiver) || !(#handle in receiver)) {
        throw new TypeError(kIllegalInvocation);
      }
      return receiver.#handle;
    };
    endTagRewrite = (receiver): Rewrite => (receiver as EndTag).#rewrite;
  }

  constructor(key: unknown, handle: HtmlEndTagHandle, rewrite: Rewrite) {
    if (key !== kConstruct) throw new TypeError(kIllegalConstructor);
    this.#handle = handle;
    this.#rewrite = rewrite;
    ObjectDefineProperties(this, kEndTagProperties);
  }

  before(content: unknown, options?: unknown): this {
    const handle = endTagHandle(this);
    rewriteContent(
      handle,
      endTagRewrite(this),
      'before',
      'EndTag',
      content,
      options
    );
    return this;
  }

  after(content: unknown, options?: unknown): this {
    const handle = endTagHandle(this);
    rewriteContent(
      handle,
      endTagRewrite(this),
      'after',
      'EndTag',
      content,
      options
    );
    return this;
  }

  remove(): this {
    endTagHandle(this).remove();
    return this;
  }
}

const kEndTagProperties = ObjectFreeze({
  __proto__: kNullProtoObject,
  name: accessor(
    function (this: unknown): string {
      return endTagHandle(this).name();
    },
    function (this: unknown, name: unknown): void {
      endTagHandle(this).setName(`${name as string}`);
    }
  ),
}) as PropertyDescriptorMap;

let commentHandle: (receiver: unknown) => HtmlCommentHandle;

class Comment {
  #handle: HtmlCommentHandle;

  static {
    commentHandle = (receiver): HtmlCommentHandle => {
      if (!isObject(receiver) || !(#handle in receiver)) {
        throw new TypeError(kIllegalInvocation);
      }
      return receiver.#handle;
    };
  }

  constructor(key: unknown, handle: HtmlCommentHandle) {
    if (key !== kConstruct) throw new TypeError(kIllegalConstructor);
    this.#handle = handle;
    ObjectDefineProperties(this, kCommentProperties);
  }

  before(content: unknown, options?: unknown): this {
    const handle = commentHandle(this);
    const text = readStringContent(content);
    handle.before(text, readHtml(options, 'before', 'Comment'));
    return this;
  }

  after(content: unknown, options?: unknown): this {
    const handle = commentHandle(this);
    const text = readStringContent(content);
    handle.after(text, readHtml(options, 'after', 'Comment'));
    return this;
  }

  replace(content: unknown, options?: unknown): this {
    const handle = commentHandle(this);
    const text = readStringContent(content);
    handle.replace(text, readHtml(options, 'replace', 'Comment'));
    return this;
  }

  remove(): this {
    commentHandle(this).remove();
    return this;
  }
}

const kCommentProperties = ObjectFreeze({
  __proto__: kNullProtoObject,
  text: accessor(
    function (this: unknown): string {
      return commentHandle(this).text();
    },
    function (this: unknown, text: unknown): void {
      commentHandle(this).setText(`${text as string}`);
    }
  ),
  removed: accessor(function (this: unknown): boolean {
    return commentHandle(this).removed();
  }),
}) as PropertyDescriptorMap;

let textHandle: (receiver: unknown) => HtmlTextHandle;
let textRewrite: (receiver: unknown) => Rewrite;

class Text {
  #handle: HtmlTextHandle;
  #rewrite: Rewrite;

  static {
    textHandle = (receiver): HtmlTextHandle => {
      if (!isObject(receiver) || !(#handle in receiver)) {
        throw new TypeError(kIllegalInvocation);
      }
      return receiver.#handle;
    };
    textRewrite = (receiver): Rewrite => (receiver as Text).#rewrite;
  }

  constructor(key: unknown, handle: HtmlTextHandle, rewrite: Rewrite) {
    if (key !== kConstruct) throw new TypeError(kIllegalConstructor);
    this.#handle = handle;
    this.#rewrite = rewrite;
    ObjectDefineProperties(this, kTextProperties);
  }

  before(content: unknown, options?: unknown): this {
    const handle = textHandle(this);
    rewriteContent(
      handle,
      textRewrite(this),
      'before',
      'Text',
      content,
      options
    );
    return this;
  }

  after(content: unknown, options?: unknown): this {
    const handle = textHandle(this);
    rewriteContent(
      handle,
      textRewrite(this),
      'after',
      'Text',
      content,
      options
    );
    return this;
  }

  replace(content: unknown, options?: unknown): this {
    const handle = textHandle(this);
    rewriteContent(
      handle,
      textRewrite(this),
      'replace',
      'Text',
      content,
      options
    );
    return this;
  }

  remove(): this {
    textHandle(this).remove();
    return this;
  }
}

const kTextProperties = ObjectFreeze({
  __proto__: kNullProtoObject,
  text: accessor(function (this: unknown): string {
    return textHandle(this).text();
  }),
  lastInTextNode: accessor(function (this: unknown): boolean {
    return textHandle(this).lastInTextNode();
  }),
  removed: accessor(function (this: unknown): boolean {
    return textHandle(this).removed();
  }),
}) as PropertyDescriptorMap;

let doctypeHandle: (receiver: unknown) => HtmlDoctypeHandle;

class Doctype {
  #handle: HtmlDoctypeHandle;

  static {
    doctypeHandle = (receiver): HtmlDoctypeHandle => {
      if (!isObject(receiver) || !(#handle in receiver)) {
        throw new TypeError(kIllegalInvocation);
      }
      return receiver.#handle;
    };
  }

  constructor(key: unknown, handle: HtmlDoctypeHandle) {
    if (key !== kConstruct) throw new TypeError(kIllegalConstructor);
    this.#handle = handle;
    ObjectDefineProperties(this, kDoctypeProperties);
  }
}

const kDoctypeProperties = ObjectFreeze({
  __proto__: kNullProtoObject,
  name: accessor(function (this: unknown): string | null {
    return doctypeHandle(this).name() ?? null;
  }),
  publicId: accessor(function (this: unknown): string | null {
    return doctypeHandle(this).publicId() ?? null;
  }),
  systemId: accessor(function (this: unknown): string | null {
    return doctypeHandle(this).systemId() ?? null;
  }),
}) as PropertyDescriptorMap;

let documentEndHandle: (receiver: unknown) => HtmlDocumentEndHandle;

class DocumentEnd {
  #handle: HtmlDocumentEndHandle;

  static {
    documentEndHandle = (receiver): HtmlDocumentEndHandle => {
      if (!isObject(receiver) || !(#handle in receiver)) {
        throw new TypeError(kIllegalInvocation);
      }
      return receiver.#handle;
    };
  }

  constructor(key: unknown, handle: HtmlDocumentEndHandle) {
    if (key !== kConstruct) throw new TypeError(kIllegalConstructor);
    this.#handle = handle;
  }

  append(content: unknown, options?: unknown): this {
    const handle = documentEndHandle(this);
    const text = readStringContent(content);
    handle.append(text, readHtml(options, 'append', 'DocumentEnd'));
    return this;
  }
}

function newElement(token: unknown, rewrite: Rewrite): Element {
  return new Element(kConstruct, token as HtmlElementHandle, rewrite);
}
function newEndTag(token: unknown, rewrite: Rewrite): EndTag {
  return new EndTag(kConstruct, token as HtmlEndTagHandle, rewrite);
}
function newComment(token: unknown): Comment {
  return new Comment(kConstruct, token as HtmlCommentHandle);
}
function newText(token: unknown, rewrite: Rewrite): Text {
  return new Text(kConstruct, token as HtmlTextHandle, rewrite);
}
function newDoctype(token: unknown): Doctype {
  return new Doctype(kConstruct, token as HtmlDoctypeHandle);
}
function newDocumentEnd(token: unknown): DocumentEnd {
  return new DocumentEnd(kConstruct, token as HtmlDocumentEndHandle);
}

// An own property of each token, as a JSG instance property is: enumerable
// and configurable, with anonymous functions of length 0.
function accessor(
  get: (this: unknown) => unknown,
  set?: (this: unknown, value: unknown) => void
): PropertyDescriptor {
  setNameAndLength(get, '', 0);
  if (set !== undefined) setNameAndLength(set, '', 0);
  return ObjectFreeze({
    __proto__: kNullProtoObject,
    get,
    set,
    enumerable: true,
    configurable: true,
  }) as PropertyDescriptor;
}

// ---------------------------------------------------------------------------
// The rest makes the classes look as JSG's do.

const kSetToStringTag = compatFlags['set_tostring_tag'] === true;

// JSG methods are enumerable, and lengths count no arguments unless
// spec_compliant_property_attributes.
function jsgShape(
  ctor: { prototype: object },
  name: string,
  methods: readonly string[]
): void {
  const proto = ctor.prototype;
  for (let i = 0; i < methods.length; i++) {
    const method = methods[i] as string;
    const descriptor = ObjectGetOwnPropertyDescriptor(
      proto,
      method
    ) as PropertyDescriptor;
    if (compatFlags['spec_compliant_property_attributes'] !== true) {
      setNameAndLength(descriptor.value, method, 0);
    }
    descriptor.enumerable = true;
    ObjectDefineProperty(proto, method, descriptor);
  }
  if (kSetToStringTag) {
    ObjectDefineProperty(proto, SymbolToStringTag, {
      __proto__: kNullProtoObject,
      value: name,
      writable: true,
      enumerable: false,
      configurable: true,
    } as PropertyDescriptor);
  }
}

jsgShape(HTMLRewriter, 'HTMLRewriter', ['on', 'onDocument', 'transform']);
jsgShape(Element, 'Element', [
  'getAttribute',
  'hasAttribute',
  'setAttribute',
  'removeAttribute',
  'before',
  'after',
  'prepend',
  'append',
  'replace',
  'remove',
  'removeAndKeepContent',
  'setInnerContent',
  'onEndTag',
]);
jsgShape(AttributesIterator, 'AttributesIterator', ['next']);
jsgShape(EndTag, 'EndTag', ['before', 'after', 'remove']);
jsgShape(Comment, 'Comment', ['before', 'after', 'replace', 'remove']);
jsgShape(Text, 'Text', ['before', 'after', 'replace', 'remove']);
jsgShape(Doctype, 'Doctype', []);
jsgShape(DocumentEnd, 'DocumentEnd', ['append']);

export type HTMLRewriterExports = {
  HTMLRewriter: typeof HTMLRewriter;
};

module.exports = {
  HTMLRewriter,
};
