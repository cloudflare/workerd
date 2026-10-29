declare namespace TailStream {

interface Header {
  readonly name: string;
  readonly value: string;
}

interface FetchEventInfo {
  readonly type: "fetch";
  readonly method: string;
  readonly url: string;
  readonly cfJson?: object;
  readonly headers: Header[];
}

interface JsRpcEventInfo {
  readonly type: "jsrpc";
}

interface ScheduledEventInfo {
  readonly type: "scheduled";
  readonly scheduledTime: Date;
  readonly cron: string;
}

interface AlarmEventInfo {
  readonly type: "alarm";
  readonly scheduledTime: Date;
}

interface QueueEventInfo {
  readonly type: "queue";
  readonly queueName: string;
  readonly batchSize: number;
}

interface EmailEventInfo {
  readonly type: "email";
  readonly mailFrom: string;
  readonly rcptTo: string;
  readonly rawSize: number;
}

interface TraceEventInfo {
  readonly type: "trace";
  readonly traces: (string | null)[];
}

interface HibernatableWebSocketEventInfoMessage {
  readonly type: "message";
}
interface HibernatableWebSocketEventInfoError {
  readonly type: "error";
}
interface HibernatableWebSocketEventInfoClose {
  readonly type: "close";
  readonly code: number;
  readonly wasClean: boolean;
}

interface HibernatableWebSocketEventInfo {
  readonly type: "hibernatableWebSocket";
  readonly info: HibernatableWebSocketEventInfoClose |
                 HibernatableWebSocketEventInfoError |
                 HibernatableWebSocketEventInfoMessage;
}

interface CustomEventInfo {
  readonly type: "custom";
}

interface FetchResponseInfo {
  readonly type: "fetch";
  readonly statusCode: number;
}

interface ConnectEventInfo {
  readonly type: "connect";
}

type EventOutcome = "ok" | "canceled" | "exception" | "unknown" | "killSwitch" |
                    "daemonDown" | "exceededCpu" | "exceededMemory" | "loadShed" |
                     "responseStreamDisconnected" | "scriptNotFound" | "internalError" |
                     "exceededWallTime" | "aborted";

interface ScriptVersion {
  readonly id?: string;
  readonly tag?: string;
  readonly message?: string;
}

interface TracePreviewInfo {
  readonly id: string;
  readonly slug: string;
  readonly name: string;
}

interface Onset {
  readonly type: "onset";
  readonly attributes?: Attribute[];
  // id for the span being opened by this Onset event.
  readonly spanId: string;
  readonly dispatchNamespace?: string;
  readonly entrypoint?: string;
  readonly executionModel: string;
  readonly durableObjectId?: string;
  readonly scriptName?: string;
  readonly scriptTags?: string[];
  readonly scriptVersion?: ScriptVersion;
  readonly preview?: TracePreviewInfo;
  readonly info: FetchEventInfo | ConnectEventInfo | JsRpcEventInfo |
                 ScheduledEventInfo | AlarmEventInfo | QueueEventInfo |
                 EmailEventInfo | TraceEventInfo |
                 HibernatableWebSocketEventInfo | CustomEventInfo;
}

interface Outcome {
  readonly type: "outcome";
  readonly outcome: EventOutcome;
  readonly cpuTime: number;
  readonly wallTime: number;
}

type SpanStatusCode = "unset" | "ok" | "error";

interface SpanStatus {
  readonly code: SpanStatusCode;
  /** A developer-facing error message, present only when code is "error". */
  readonly message?: string;
}

/** An application-recorded OpenTelemetry event on the current span. */
interface SpanEvent {
  readonly type: 'spanEvent';
  readonly name: string;
  readonly attributes: Attribute[];
  /** Explicit Unix timestamp as [seconds, nanoseconds]; otherwise use the tail timestamp. */
  readonly time?: [number, number];
}

/** A link to another local or remote OpenTelemetry span. */
interface SpanLink {
  readonly context: {
    readonly traceId: string;
    readonly spanId: string;
    readonly traceFlags: number;
    readonly isRemote: boolean;
    readonly traceState?: string;
  };
  readonly attributes: Attribute[];
}

/** Links appended to the span identified by the outer tail event context. */
interface SpanLinks {
  readonly type: 'spanLinks';
  readonly links: SpanLink[];
}

interface SpanOpen {
  /** R3/R4/R8: runtime-only adapter root, behind opentelemetry_api. Present only when true;
   * spanContext.spanId is absent and traceId differs from onset. Unmarked parentless opens
   * are malformed. A remote parent outside the stream is allowed (R5), not a root. */
  readonly root?: true;
  /** Explicit application start time as [Unix seconds, nanoseconds]. */
  readonly startTime?: [number, number];
  /** OpenTelemetry span kind, when supplied by the instrumentation. */
  kind?: 'internal' | 'server' | 'client' | 'producer' | 'consumer';
  /** The instrumentation that created this span. Carried only on spanOpen. */
  scope?: {
    name: string;
    version?: string;
    schemaUrl?: string;
  };
  readonly type: "spanOpen";
  readonly name: string;
  // id for the span being opened by this SpanOpen event.
  readonly spanId: string;
  readonly info?: FetchEventInfo | JsRpcEventInfo | Attributes;
}

interface SpanClose {
  /** Explicit application end time as [Unix seconds, nanoseconds]. */
  readonly endTime?: [number, number];
  readonly type: "spanClose";
  readonly outcome: EventOutcome;
}

type SpanUpdateInfo =
  | { readonly type: "name"; readonly name: string }
  | { readonly type: "status"; readonly status: SpanStatus };

interface SpanUpdate {
  readonly type: "spanUpdate";
  readonly info: SpanUpdateInfo;
}

interface DiagnosticChannelEvent {
  readonly type: "diagnosticChannel";
  readonly channel: string;
  readonly message: any;
}

interface Exception {
  /** Explicit application event time as [Unix seconds, nanoseconds]. */
  readonly time?: [number, number];
  readonly type: "exception";
  readonly code?: string | number;
  readonly name: string;
  readonly message: string;
  readonly stack?: string;
}

interface TailStreamErrorInfo {
  readonly name: string;
  readonly message: string;
  readonly stack?: string;
}

type Log = {
  readonly type: "log";
  readonly level: "debug" | "error" | "info" | "log" | "warn";
  readonly errorInfo?: readonly (TailStreamErrorInfo | null)[];
} & (
  | { readonly message: object; readonly truncated?: false }
  | { readonly message: string; readonly truncated: true }
);

interface DroppedEventsDiagnostic {
  readonly diagnosticsType: "droppedEvents";
  readonly count: number;
}

interface StreamDiagnostic {
  readonly type: 'streamDiagnostic';
  // To add new diagnostic types, define a new interface and add it to this union type.
  readonly diagnostic: DroppedEventsDiagnostic;
}

// This marks the worker handler return information.
// This is separate from Outcome because the worker invocation can live for a long time after
// returning. For example - Websockets that return an http upgrade response but then continue
// streaming information or SSE http connections.
interface Return {
  readonly type: "return";
  readonly info?: FetchResponseInfo;
}

interface Attribute {
  readonly name: string;
  readonly value:
    | string
    | (string | null)[]
    | boolean
    | (boolean | null)[]
    | number
    | (number | null)[]
    | bigint
    | bigint[];
}

interface Attributes {
  readonly type: "attributes";
  readonly info: Attribute[];
}

type EventType =
  | Onset
  | Outcome
  | SpanOpen
  | SpanEvent
  | SpanLinks
  | SpanClose
  | SpanUpdate
  | DiagnosticChannelEvent
  | Exception
  | Log
  | StreamDiagnostic
  | Return
  | Attributes;

// Context in which this trace event lives.
interface SpanContext {
  // R1: trace of this event's span, not necessarily the onset's trace. Immutable per span;
  // an open whose parent is in the stream carries the parent's trace.
  readonly traceId: string;
  // spanId in which this event is handled
  // for Onset and SpanOpen events this would be the parent span id
  // for Outcome and SpanClose these this would be the span id of the opening Onset and SpanOpen events
  // For Hibernate and Mark this would be the span under which they were emitted.
  // R3: absent only on onset without inherited context, or spanOpen with root: true.
  readonly spanId?: string;
  // W3C trace flags from an upstream traceparent. Absent when no upstream
  // sampling decision was made.
  // R6: detached subtrees inherit onset flags verbatim, including absence.
  readonly traceFlags?: number;
}

interface TailEvent<Event extends EventType> {
  // R2: group by invocationId, not traceId. Multiple traces share one invocation sequence
  // space; span IDs are unique within it. Consumers key spans by ID within the invocation.
  // R7: force-close all open spans at outcome, regardless of trace.
  // invocation id of the currently invoked worker stage.
  // invocation id will always be unique to every Onset event and will be the same until the Outcome event.
  readonly invocationId: string;
  // Inherited spanContext for this event.
  readonly spanContext: SpanContext;
  readonly timestamp: Date;
  readonly sequence: number;
  readonly event: Event;
}

type TailEventHandler<Event extends EventType = EventType> = (
  event: TailEvent<Event>
) => void | Promise<void>;

type TailEventHandlerObject = {
  outcome?: TailEventHandler<Outcome>;
  spanOpen?: TailEventHandler<SpanOpen>;
  spanEvent?: TailEventHandler<SpanEvent>;
  spanLinks?: TailEventHandler<SpanLinks>;
  spanClose?: TailEventHandler<SpanClose>;
  spanUpdate?: TailEventHandler<SpanUpdate>;
  diagnosticChannel?: TailEventHandler<DiagnosticChannelEvent>;
  exception?: TailEventHandler<Exception>;
  log?: TailEventHandler<Log>;
  return?: TailEventHandler<Return>;
  attributes?: TailEventHandler<Attributes>;
};

type TailEventHandlerType = TailEventHandler | TailEventHandlerObject;
}
