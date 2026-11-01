'use strict';

// EventSource, under the TS_EVENT_SOURCE autogate. It replaces the C++ class
// in api/eventsource.h, keeping its Workers-specific behaviors: the
// EventSourceInit `fetcher` extension, EventSource.from(), disposal through
// close(), absolute URLs only (there is no document base URL), a reconnection
// time clamped to between 1 and 10 seconds, failing rather than reconnecting
// when fetch() rejects, and one reconnection attempt after a response without
// a body. src/tests/events/eventsource/ runs one suite against both
// implementations, and its AGENTS.md records where each difference below is
// pinned.
//
// DIFFERENCES FROM THE C++ IMPLEMENTATION
//
// - The constructor inherits from EventTarget's, as WebIDL requires, and its
//   `prototype` is non-writable. Calling it without `new` throws the
//   language's TypeError rather than JSG's.
// - The event stream is parsed as the HTML standard specifies, where the C++
//   parser departs from it: an empty `event` field dispatches a 'message'
//   (C++: an event of type ''); a BOM is stripped however the first chunks
//   split it; an `id` field containing U+0000 is ignored; a `retry` field
//   counts only if it is all ASCII digits; a `data` field with an empty value
//   dispatches a message with empty data (C++: nothing); a CRLF split across
//   chunks ends one line (C++: two, dispatching early); and the last event ID
//   that a reconnection sends is the one of the last dispatched event, not
//   the last `id` field parsed.
// - Each message is dispatched only while the EventSource is not closed, so
//   closing it from a listener drops the rest of the messages read with it.
// - close(), and an error that closes it, cancel the stream of an
//   EventSource.from() source. The C++ one goes on reading it.
// - Messages are dispatched from the reaction to the read that produced
//   them, rather than from a task the C++ sink queues for each write. In a
//   Durable Object they are therefore not held back by the input gate.
//
// HOW IT FITS TOGETHER
//
// The EventSource extends the C++ EventTarget through
// events/event-target-internals, and dispatches the events the C++ one does
// with the same exception policies: an 'open' or 'message' listener
// exception errors the EventSource out (fail fast) and an 'error' listener's
// is only reported. Its I/O goes through an EventSourceIo (utils), bound to
// the request that created it: every continuation first checks that it runs
// in that request, and throws as the C++ one does if not. The stream is read
// with a reader of whichever streams implementation is installed, captured
// below, and decoded with a streaming TextDecoder before it is parsed.

import type { EventTargetInternals } from '../events/event-target-internals';

const {
  AbortController,
  AbortControllerAbort,
  AbortControllerSignalGet,
  ArrayPrototypePush,
  DOMException,
  DataViewPrototypeGetBuffer,
  DataViewPrototypeGetByteLength,
  DataViewPrototypeGetByteOffset,
  Error,
  MathMax,
  MathMin,
  Number,
  ObjectDefineProperties,
  ObjectDefineProperty,
  ObjectFreeze,
  ObjectGetOwnPropertyDescriptor,
  PromisePrototypeThen,
  StringPrototypeCharCodeAt,
  StringPrototypeIndexOf,
  StringPrototypeSlice,
  Symbol,
  SymbolDispose,
  SymbolFor,
  SymbolToStringTag,
  TextDecoder,
  TextDecoderDecode,
  TextEncoder,
  TextEncoderEncode,
  TypeError,
  TypedArrayPrototypeGetBuffer,
  TypedArrayPrototypeGetByteLength,
  TypedArrayPrototypeGetByteOffset,
  Uint8Array,
  captureJsgGetter,
  uncurryThis,
} = primordials;

const {
  createEventSourceIo,
  getApiSymbol,
  isAnyArrayBuffer,
  isArrayBufferView,
  isDataView,
  isEventStreamMimeType,
  isFetcher,
  markPromiseHandled,
  parseUrl,
  readableStreamState,
} = utils;

const {
  EventTarget,
  createTrustedErrorEvent,
  createTrustedMessageEvent,
  createTrustedOpenEvent,
  dispatchEvent,
  effectiveExceptionPolicy,
  getEventHandlerAttribute,
  setEventHandlerAttribute,
} = require('events/event-target-internals') as EventTargetInternals;

// Whichever streams implementation main.ts installed before loading this
// module; a Response body and anything EventSource.from() accepts are its
// streams.
type Reader = ReadableStreamDefaultReader<unknown>;
const ReadableStreamGetReader = uncurryThis(
  globalThis.ReadableStream.prototype.getReader
) as (stream: ReadableStream) => Reader;
const ReaderRead = uncurryThis(
  globalThis.ReadableStreamDefaultReader.prototype.read
) as (reader: Reader) => Promise<ReadableStreamReadResult<unknown>>;
const ReaderCancel = uncurryThis(
  globalThis.ReadableStreamDefaultReader.prototype.cancel
) as (reader: Reader, reason?: unknown) => Promise<void>;

const ResponseProto = globalThis.Response.prototype;
const ResponseOkGet = captureJsgGetter<(response: Response) => boolean>(
  ResponseProto,
  'ok'
);
const ResponseStatusGet = captureJsgGetter<(response: Response) => number>(
  ResponseProto,
  'status'
);
const ResponseHeadersGet = captureJsgGetter<(response: Response) => Headers>(
  ResponseProto,
  'headers'
);
const ResponseRedirectedGet = captureJsgGetter<(response: Response) => boolean>(
  ResponseProto,
  'redirected'
);
const ResponseUrlGet = captureJsgGetter<(response: Response) => string>(
  ResponseProto,
  'url'
);
const ResponseBodyGet = captureJsgGetter<
  (response: Response) => ReadableStream | null
>(ResponseProto, 'body');
const HeadersGet = uncurryThis(globalThis.Headers.prototype.get) as (
  headers: Headers,
  name: string
) => string | null;

// JSG's messages, so that misuse reads the same as with the C++ class.
const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';
const kNoRequest =
  'An EventSource can only be created within the context of a worker request.';

const CONNECTING = 0;
const OPEN = 1;
const CLOSED = 2;
type ReadyState = typeof CONNECTING | typeof OPEN | typeof CLOSED;

const kDefaultReconnectionTime = 2000;
const kMinReconnectionTime = 1000;
const kMaxReconnectionTime = 10000;

const kLF = 0x0a;
const kColon = 0x3a;
const kSpace = 0x20;
const kDigit0 = 0x30;
const kDigit9 = 0x39;

const kStreamOption = ObjectFreeze({ __proto__: null, stream: true });

// What the C++ implementation's read of each streams implementation throws
// for a chunk that is not bytes.
const kNotBytes =
  compatFlags['typescript_implemented_streams'] === true
    ? 'This ReadableStream did not return bytes.'
    : 'Draining read encountered a value that cannot be converted to bytes';

// Lets EventSource.from() construct an EventSource without a URL.
const kConstructFrom: symbol = Symbol('kConstructFrom');

type Message = {
  readonly type: string;
  readonly data: string;
  readonly lastEventId: string;
};

function isAsciiDigits(value: string): boolean {
  if (value.length === 0) return false;
  for (let i = 0; i < value.length; i++) {
    const c = StringPrototypeCharCodeAt(value, i);
    if (c < kDigit0 || c > kDigit9) return false;
  }
  return true;
}

// The HTML standard's event stream interpretation, over decoded text: lines,
// fields, and the dispatch of each event they complete.
class EventStreamParser {
  // The text of the line in progress, and whether the last text ended in a
  // CR, so that an LF beginning the next is part of the same line ending.
  #partial = '';
  #afterCR = false;
  #data = '';
  #eventType = '';
  #lastEventIdBuffer: string;
  // The event source's last event ID string, set as each event dispatches.
  lastEventId: string;
  // The reconnection time a `retry` field last set, else undefined.
  retry: number | undefined = undefined;

  constructor(lastEventId: string) {
    this.#lastEventIdBuffer = lastEventId;
    this.lastEventId = lastEventId;
  }

  // Parses `text`, appending the messages it completes to `messages`.
  feed(text: string, messages: Message[]): void {
    let start = 0;
    if (this.#afterCR) {
      this.#afterCR = false;
      if (text.length > 0 && StringPrototypeCharCodeAt(text, 0) === kLF) {
        start = 1;
      }
    }
    // The next LF and CR at or after `start`, searched for again only once
    // `start` passes them, so that text with one kind of line ending is not
    // rescanned for the other on every line. Not found stays not found.
    let lf = StringPrototypeIndexOf(text, '\n', start);
    let cr = StringPrototypeIndexOf(text, '\r', start);
    while (start < text.length) {
      if (lf !== -1 && lf < start)
        lf = StringPrototypeIndexOf(text, '\n', start);
      if (cr !== -1 && cr < start)
        cr = StringPrototypeIndexOf(text, '\r', start);
      let end: number;
      if (cr === -1) end = lf;
      else if (lf === -1) end = cr;
      else end = cr < lf ? cr : lf;
      if (end === -1) break;
      const line = this.#partial + StringPrototypeSlice(text, start, end);
      this.#partial = '';
      this.#processLine(line, messages);
      start = end + 1;
      if (end === cr) {
        if (start === text.length) {
          this.#afterCR = true;
        } else if (StringPrototypeCharCodeAt(text, start) === kLF) {
          start++;
        }
      }
    }
    if (start < text.length) {
      this.#partial += StringPrototypeSlice(text, start);
    }
  }

  #processLine(line: string, messages: Message[]): void {
    if (line.length === 0) {
      this.#dispatch(messages);
      return;
    }
    if (StringPrototypeCharCodeAt(line, 0) === kColon) return;
    const colon = StringPrototypeIndexOf(line, ':');
    let field: string;
    let value: string;
    if (colon === -1) {
      field = line;
      value = '';
    } else {
      field = StringPrototypeSlice(line, 0, colon);
      const valueStart =
        colon + 1 < line.length &&
        StringPrototypeCharCodeAt(line, colon + 1) === kSpace
          ? colon + 2
          : colon + 1;
      value = StringPrototypeSlice(line, valueStart);
    }
    switch (field) {
      case 'event':
        this.#eventType = value;
        break;
      case 'data':
        this.#data += value + '\n';
        break;
      case 'id':
        if (StringPrototypeIndexOf(value, '\0') === -1) {
          this.#lastEventIdBuffer = value;
        }
        break;
      case 'retry':
        if (isAsciiDigits(value)) {
          this.retry = MathMin(
            MathMax(Number(value), kMinReconnectionTime),
            kMaxReconnectionTime
          );
        }
        break;
    }
  }

  #dispatch(messages: Message[]): void {
    this.lastEventId = this.#lastEventIdBuffer;
    if (this.#data.length === 0) {
      this.#eventType = '';
      return;
    }
    const data = StringPrototypeSlice(this.#data, 0, this.#data.length - 1);
    const type = this.#eventType.length === 0 ? 'message' : this.#eventType;
    this.#data = '';
    this.#eventType = '';
    ArrayPrototypePush(messages, {
      __proto__: null,
      type,
      data,
      lastEventId: this.lastEventId,
    });
  }
}

const encoder = new TextEncoder();

// A chunk as bytes, as the C++ implementation's draining read converts it:
// a string as its UTF-8, any buffer or view as its bytes. Undefined for
// anything else.
function chunkBytes(chunk: unknown): Uint8Array | undefined {
  if (typeof chunk === 'string') return TextEncoderEncode(encoder, chunk);
  if (isAnyArrayBuffer(chunk)) return new Uint8Array(chunk);
  if (isArrayBufferView(chunk)) {
    if (isDataView(chunk)) {
      return new Uint8Array(
        DataViewPrototypeGetBuffer(chunk),
        DataViewPrototypeGetByteOffset(chunk),
        DataViewPrototypeGetByteLength(chunk)
      );
    }
    return new Uint8Array(
      TypedArrayPrototypeGetBuffer(chunk),
      TypedArrayPrototypeGetByteOffset(chunk),
      TypedArrayPrototypeGetByteLength(chunk)
    );
  }
  return undefined;
}

// Whether a read failure is a disconnection, as JSG classifies a JavaScript
// exception it converts (DISCONNECTED: `retryable` but not `overloaded`),
// which the C++ implementation treats as the end of the stream.
function isDisconnection(error: unknown): boolean {
  if (
    (typeof error !== 'object' && typeof error !== 'function') ||
    error === null
  ) {
    return false;
  }
  const { overloaded, retryable } = error as {
    overloaded?: unknown;
    retryable?: unknown;
  };
  return !overloaded && !!retryable;
}

function ignoreRejection(promise: Promise<unknown>): void {
  markPromiseHandled(promise);
}

// The brand check of every EventSource member: any receiver that is not an
// EventSource, primitives and undefined included, is an illegal invocation.
let requireSource: (receiver: unknown) => void;

class EventSource extends EventTarget {
  #io: EventSourceIo;
  #readyState: ReadyState = CONNECTING;
  #closeCalled = false;
  // Undefined for an EventSource.from() source.
  #url: string | undefined = undefined;
  #origin: string | null = null;
  #fetcher: unknown = undefined;
  #controller: AbortController = new AbortController();
  // The reader of the stream being read, if any.
  #reader: Reader | undefined = undefined;
  #lastEventId = '';
  #reconnectionTime = kDefaultReconnectionTime;
  // Set once a response has had no body; a second one fails the connection.
  #previousNoBody = false;

  static {
    requireSource = (receiver): void => {
      if (
        typeof receiver !== 'object' ||
        receiver === null ||
        !(#readyState in receiver)
      ) {
        throw new TypeError(kIllegalInvocation);
      }
    };
  }

  constructor(url?: unknown, init?: unknown) {
    if (url === kConstructFrom) {
      super();
      this.#io = init as EventSourceIo;
      return;
    }

    // Converted as JSG converts the C++ constructor's arguments, before
    // anything is checked.
    if (url === undefined) {
      throw new TypeError(
        "Failed to construct 'EventSource': constructor parameter 1 is not " +
          "of type 'string'."
      );
    }
    const urlString = `${url as string}`;
    let withCredentials = false;
    let fetcher: unknown = undefined;
    if (init !== undefined && init !== null) {
      if (typeof init !== 'object' && typeof init !== 'function') {
        throw new TypeError(
          "Failed to construct 'EventSource': constructor parameter 2 is " +
            "not of type 'EventSourceInit'."
        );
      }
      withCredentials = !!(init as { withCredentials?: unknown })
        .withCredentials;
      fetcher = (init as { fetcher?: unknown }).fetcher;
      if (fetcher !== undefined && !isFetcher(fetcher)) {
        throw new TypeError(
          "Incorrect type for the 'fetcher' field on 'EventSourceInit': " +
            "the provided value is not of type 'Fetcher'."
        );
      }
    }

    const io = createEventSourceIo();
    if (io === undefined)
      throw new DOMException(kNoRequest, 'NotSupportedError');
    if (withCredentials) {
      throw new DOMException(
        'The init.withCredentials option is not supported. It must be ' +
          'false or undefined.',
        'NotSupportedError'
      );
    }
    const parsed = parseUrl(urlString);
    if (parsed === undefined) {
      throw new DOMException(
        `Cannot open an EventSource to '${urlString}'. The URL is invalid.`,
        'SyntaxError'
      );
    }

    super();
    this.#io = io;
    this.#url = parsed.href;
    this.#origin = parsed.origin;
    this.#fetcher = fetcher;
    this.#connect();
  }

  // Defined as a static method below, after the constants, as JSG orders it.
  static fromStream(stream: unknown): EventSource {
    const state = readableStreamState(stream);
    if (state === undefined) {
      throw new TypeError(
        "Failed to execute 'from' on 'EventSource': parameter 1 is not of " +
          "type 'JsReadableStream'."
      );
    }
    const io = createEventSourceIo();
    if (io === undefined)
      throw new DOMException(kNoRequest, 'NotSupportedError');
    if (state === 'locked') {
      throw new TypeError('This ReadableStream is locked.');
    }
    if (state === 'disturbed') {
      throw new TypeError('This ReadableStream has already been read from.');
    }
    const source = new EventSource(kConstructFrom, io);
    source.#read(stream as ReadableStream, false);
    return source;
  }

  close(): void {
    requireSource(this);
    if (this.#closeCalled) return;
    this.#io.requireCurrent();
    this.#closeCalled = true;
    AbortControllerAbort(this.#controller);
    this.#readyState = CLOSED;
    this.#cancelReader(undefined);
  }

  get url(): string {
    requireSource(this);
    return this.#url ?? '';
  }

  get withCredentials(): boolean {
    requireSource(this);
    return false;
  }

  get readyState(): number {
    requireSource(this);
    return this.#readyState;
  }

  get onopen(): unknown {
    requireSource(this);
    return getEventHandlerAttribute(this, 'open');
  }

  set onopen(value: unknown) {
    requireSource(this);
    setEventHandlerAttribute(this, 'open', value);
  }

  get onmessage(): unknown {
    requireSource(this);
    return getEventHandlerAttribute(this, 'message');
  }

  set onmessage(value: unknown) {
    requireSource(this);
    setEventHandlerAttribute(this, 'message', value);
  }

  get onerror(): unknown {
    requireSource(this);
    return getEventHandlerAttribute(this, 'error');
  }

  set onerror(value: unknown) {
    requireSource(this);
    setEventHandlerAttribute(this, 'error', value);
  }

  // -------------------------------------------------------------------------
  // The connection.

  #connect(): void {
    this.#io.requireCurrent();
    if (this.#readyState === CLOSED) return;
    const response = this.#io.fetch(
      this.#fetcher,
      this.#url as string,
      this.#lastEventId,
      AbortControllerSignalGet(this.#controller)
    );
    PromisePrototypeThen(
      response,
      (value: Response) => {
        this.#onResponse(value);
      },
      (error: unknown) => {
        this.#io.requireCurrent();
        this.#notifyError(error);
      }
    );
  }

  #onResponse(response: Response): void {
    this.#io.requireCurrent();
    if (this.#readyState === CLOSED) return;
    // Failures that no reconnection could cure.
    const fail = (message: string): void => {
      this.#notifyError(new DOMException(message, 'AbortError'));
    };
    if (!ResponseOkGet(response)) {
      fail(`The response status code was ${ResponseStatusGet(response)}.`);
      return;
    }
    const contentType = HeadersGet(
      ResponseHeadersGet(response),
      'content-type'
    );
    if (contentType === null) {
      fail('No content type header was present in the response.');
      return;
    }
    if (!isEventStreamMimeType(contentType)) {
      fail(`The content type '${contentType}' is invalid.`);
      return;
    }
    if (ResponseRedirectedGet(response)) {
      const parsed = parseUrl(ResponseUrlGet(response));
      if (parsed !== undefined) {
        this.#url = parsed.href;
        this.#origin = parsed.origin;
      }
    }

    const body = ResponseBodyGet(response);
    if (body !== null) {
      this.#read(body, true);
    } else if (this.#previousNoBody) {
      // Likely a bug in the server, or along the way to it.
      this.#notifyError(new Error('The server provided no content.'));
    } else {
      this.#previousNoBody = true;
      this.#notifyError(
        new Error('The server provided no content. Will try reconnecting.'),
        true
      );
      this.#reconnect();
    }
  }

  #reconnect(): void {
    this.#io.requireCurrent();
    if (this.#readyState === CLOSED) return;
    this.#readyState = CONNECTING;
    this.#controller = new AbortController();
    PromisePrototypeThen(
      this.#io.wait(
        this.#reconnectionTime,
        AbortControllerSignalGet(this.#controller)
      ),
      () => {
        this.#connect();
      },
      (error: unknown) => {
        // Most likely closed while waiting: nothing more to do.
        this.#io.requireCurrent();
        this.#notifyError(error);
      }
    );
  }

  // Opens the EventSource and reads `stream` until it ends or fails, or the
  // EventSource closes.
  #read(stream: ReadableStream, withReconnection: boolean): void {
    this.#io.requireCurrent();
    this.#notifyOpen();
    if (this.#readyState === CLOSED) return;

    const reader = ReadableStreamGetReader(stream);
    this.#reader = reader;
    const decoder = new TextDecoder();
    const parser = new EventStreamParser(this.#lastEventId);
    // Whether this read is still the EventSource's, rather than one it has
    // closed or moved on from.
    const isCurrentRead = (): boolean =>
      this.#reader === reader && this.#readyState !== CLOSED;

    const onEnd = (): void => {
      this.#reader = undefined;
      this.#notifyError(
        new Error('The server disconnected.'),
        withReconnection
      );
      if (withReconnection) this.#reconnect();
    };
    const readNext = (): void => {
      PromisePrototypeThen(
        ReaderRead(reader),
        (result: ReadableStreamReadResult<unknown>) => {
          this.#io.requireCurrent();
          if (!isCurrentRead()) return;
          if (result.done) {
            onEnd();
            return;
          }
          const bytes = chunkBytes(result.value);
          if (bytes === undefined) {
            this.#notifyError(new TypeError(kNotBytes));
            return;
          }
          const messages: Message[] = [];
          parser.feed(
            TextDecoderDecode(decoder, bytes, kStreamOption),
            messages
          );
          this.#lastEventId = parser.lastEventId;
          if (parser.retry !== undefined) this.#reconnectionTime = parser.retry;
          this.#notifyMessages(messages);
          if (isCurrentRead()) readNext();
        },
        (error: unknown) => {
          this.#io.requireCurrent();
          if (!isCurrentRead()) return;
          if (isDisconnection(error)) {
            onEnd();
          } else {
            this.#notifyError(error);
          }
        }
      );
    };
    readNext();
  }

  #cancelReader(reason: unknown): void {
    const reader = this.#reader;
    if (reader === undefined) return;
    this.#reader = undefined;
    ignoreRejection(ReaderCancel(reader, reason));
  }

  // -------------------------------------------------------------------------
  // Events.

  #notifyOpen(): void {
    if (this.#readyState === CLOSED) return;
    this.#readyState = OPEN;
    const outcome = dispatchEvent(
      this,
      createTrustedOpenEvent(),
      effectiveExceptionPolicy('report')
    );
    if (outcome !== undefined) {
      // An 'open' listener threw; its exception was reported. Fail fast.
      this.#notifyError(outcome.exception, false, true);
    }
  }

  #notifyMessages(messages: Message[]): void {
    if (messages.length === 0) return;
    const origin = this.#url === undefined ? null : this.#origin;
    if (effectiveExceptionPolicy('report') === 'propagate') {
      // Without spec_compliant_dispatch_exceptions the first listener
      // exception ends the dispatch, and errors the EventSource out.
      let threw = false;
      let exception: unknown;
      try {
        for (let i = 0; i < messages.length; i++) {
          if (this.#readyState === CLOSED) return;
          this.#dispatchMessage(messages[i] as Message, origin, 'propagate');
        }
      } catch (e) {
        threw = true;
        exception = e;
      }
      if (threw) this.#notifyError(exception);
      return;
    }

    for (let i = 0; i < messages.length; i++) {
      if (this.#readyState === CLOSED) return;
      const outcome = this.#dispatchMessage(
        messages[i] as Message,
        origin,
        'report'
      );
      if (outcome !== undefined) {
        // A listener threw; its exception was reported, and the remaining
        // listeners still ran. Fail fast, dropping the remaining messages.
        this.#notifyError(outcome.exception, false, true);
        return;
      }
    }
  }

  #dispatchMessage(
    message: Message,
    origin: string | null,
    policy: 'report' | 'propagate'
  ): { readonly exception: unknown } | undefined {
    return dispatchEvent(
      this,
      createTrustedMessageEvent(
        message.type,
        message.data,
        null,
        message.lastEventId,
        origin
      ),
      policy
    );
  }

  // Errors the EventSource: aborts its connection, moves it to CONNECTING if
  // it will reconnect or else CLOSED, and fires 'error'. Unless
  // `alreadyReported`, the error is also logged as uncaught.
  #notifyError(
    error: unknown,
    reconnecting: boolean = false,
    alreadyReported: boolean = false
  ): void {
    if (this.#readyState === CLOSED) return;
    AbortControllerAbort(this.#controller, error);
    if (reconnecting) {
      this.#readyState = CONNECTING;
    } else {
      this.#readyState = CLOSED;
      this.#cancelReader(error);
    }
    // Report only: the EventSource has already errored out, so a throwing
    // 'error' listener has its exception reported and nothing more.
    dispatchEvent(
      this,
      createTrustedErrorEvent(error),
      effectiveExceptionPolicy('report')
    );
    if (!alreadyReported) this.#io.logUncaughtException(error);
  }
}

// ---------------------------------------------------------------------------
// The rest makes the class look as JSG's does.

const kSpecCompliantPropertyAttributes =
  compatFlags['spec_compliant_property_attributes'] === true;
const kInternalClass = SymbolFor('cloudflare:internal-class');
const kResourceTypeInspect = getApiSymbol('kResourceTypeInspect');
const kNoInspectProperties = ObjectFreeze({ __proto__: null });

const proto = EventSource.prototype;

function setNameAndLength(fn: unknown, name: string, length: number): void {
  ObjectDefineProperties(fn, {
    __proto__: null,
    name: { __proto__: null, value: name },
    length: { __proto__: null, value: length },
  });
}

// WebIDL constants are non-configurable; JSG's are configurable unless
// spec_compliant_property_attributes.
function constant(value: number): PropertyDescriptor {
  return {
    __proto__: null,
    value,
    writable: false,
    enumerable: true,
    configurable: !kSpecCompliantPropertyAttributes,
  } as PropertyDescriptor;
}

// JSG methods are enumerable.
ObjectDefineProperties(proto, {
  __proto__: null,
  close: { __proto__: null, enumerable: true },
});

// A JSG accessor is enumerable, its functions anonymous and of length 0,
// unless spec_compliant_property_attributes gives them their WebIDL names and
// lengths. Redefined in JSG's order: the readonly ones, then the handlers.
const kAccessors = [
  'url',
  'withCredentials',
  'readyState',
  'onopen',
  'onmessage',
  'onerror',
] as const;
for (let i = 0; i < kAccessors.length; i++) {
  const name = kAccessors[i] as string;
  const descriptor = ObjectGetOwnPropertyDescriptor(
    proto,
    name
  ) as PropertyDescriptor;
  setNameAndLength(
    descriptor.get,
    kSpecCompliantPropertyAttributes ? `get ${name}` : '',
    0
  );
  if (descriptor.set !== undefined) {
    setNameAndLength(
      descriptor.set,
      kSpecCompliantPropertyAttributes ? `set ${name}` : '',
      kSpecCompliantPropertyAttributes ? 1 : 0
    );
  }
  descriptor.enumerable = true;
  delete (proto as unknown as Record<string, unknown>)[name];
  ObjectDefineProperty(proto, name, descriptor);
}

// The constants, on the prototype and then the constructor, which then gets
// from() after them. The constructor property moves after the constants, and
// the marks every JSG resource type carries follow it.
ObjectDefineProperties(proto, {
  __proto__: null,
  CONNECTING: constant(CONNECTING),
  OPEN: constant(OPEN),
  CLOSED: constant(CLOSED),
});
delete (proto as unknown as { constructor?: unknown }).constructor;
ObjectDefineProperty(proto, 'constructor', {
  __proto__: null,
  value: EventSource,
  writable: true,
  enumerable: false,
  configurable: true,
} as PropertyDescriptor);

const from = EventSource.fromStream;
delete (EventSource as unknown as Record<string, unknown>).fromStream;
// Under spec_compliant_property_attributes, lengths count the required
// arguments.
setNameAndLength(from, 'from', kSpecCompliantPropertyAttributes ? 1 : 0);
ObjectDefineProperties(EventSource, {
  __proto__: null,
  CONNECTING: constant(CONNECTING),
  OPEN: constant(OPEN),
  CLOSED: constant(CLOSED),
  from: {
    __proto__: null,
    value: from,
    writable: true,
    enumerable: true,
    configurable: true,
  },
});
setNameAndLength(
  EventSource,
  'EventSource',
  kSpecCompliantPropertyAttributes ? 1 : 0
);

if (compatFlags['set_tostring_tag'] === true) {
  ObjectDefineProperty(proto, SymbolToStringTag, {
    __proto__: null,
    value: 'EventSource',
    writable: true,
    enumerable: false,
    configurable: true,
  } as PropertyDescriptor);
}
ObjectDefineProperties(proto, {
  __proto__: null,
  [kInternalClass]: {
    __proto__: null,
    value: kInternalClass,
    writable: false,
    enumerable: false,
    configurable: false,
  },
  [kResourceTypeInspect]: {
    __proto__: null,
    value: kNoInspectProperties,
    writable: false,
    enumerable: false,
    configurable: true,
  },
});

// Disposal defers to close(), through a function of its own, anonymous and
// of length 0, as JSG_DISPOSE installs it.
const dispose = function (this: EventSource): void {
  requireSource(this);
  this.close();
};
setNameAndLength(dispose, '', 0);
ObjectDefineProperty(proto, SymbolDispose, {
  __proto__: null,
  value: dispose,
  writable: true,
  enumerable: false,
  configurable: true,
} as PropertyDescriptor);

export type EventSourceExports = {
  EventSource: typeof EventSource;
};

module.exports = {
  EventSource,
};
