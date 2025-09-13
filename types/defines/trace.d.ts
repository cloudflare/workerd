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
  readonly methodName: string;
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

type EventOutcome = "ok" | "canceled" | "exception" | "unknown" | "killSwitch" |
                    "daemonDown" | "exceededCpu" | "exceededMemory" | "loadShed" |
                    "responseStreamDisconnected" | "scriptNotFound";

interface ScriptVersion {
  readonly id: string;
  readonly tag?: string;
  readonly message?: string;
}

interface Onset {
  readonly type: "onset";
  readonly attributes: Attribute[];
  // id for the span being opened by this Onset event.
  readonly spanId: string;
  readonly dispatchNamespace?: string;
  readonly entrypoint?: string;
  readonly executionModel: string;
  readonly scriptName?: string;
  readonly scriptTags?: string[];
  readonly scriptVersion?: ScriptVersion;
  readonly durableObjectId?: string;
  readonly info: FetchEventInfo | JsRpcEventInfo | ScheduledEventInfo |
                 AlarmEventInfo | QueueEventInfo | EmailEventInfo |
                 TraceEventInfo | HibernatableWebSocketEventInfo |
                 CustomEventInfo;
}

interface Outcome {
  readonly type: "outcome";
  readonly outcome: EventOutcome;
  readonly cpuTime: number;
  readonly wallTime: number;
}

interface SpanOpen {
  readonly type: "spanOpen";
  readonly name: string;
  // id for the span being opened by this SpanOpen event.
  readonly spanId: string;
  readonly info?: FetchEventInfo | JsRpcEventInfo | Attributes;
}

interface SpanClose {
  readonly type: "spanClose";
  readonly outcome: EventOutcome;
}

interface DiagnosticChannelEvent {
  readonly type: "diagnosticChannel";
  readonly channel: string;
  readonly message: any;
}

interface Exception {
  readonly type: "exception";
  readonly name: string;
  readonly message: string;
  readonly stack?: string;
}

interface Log {
  readonly type: "log";
  readonly level: "debug" | "error" | "info" | "log" | "warn";
  readonly message: object;
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
  readonly value: string | string[] | boolean | boolean[] | number | number[] | bigint | bigint[];
}

interface Attributes {
  readonly type: "attributes";
  readonly info: Attribute[];
}

type EventType =
  | Onset
  | Outcome
  | SpanOpen
  | SpanClose
  | DiagnosticChannelEvent
  | Exception
  | Log
  | Return
  | Attributes;

// Context in which this trace event lives.
interface SpanContext {
  // Single id for the entire top-level invocation
  // This should be a new traceId for the first worker stage invoked in the eyeball request and then
  // same-account service-bindings should reuse the same traceId but cross-account service-bindings
  // should use a new traceId.
  readonly traceId: string;
  // spanId in which this event is handled
  // for Onset and SpanOpen events this would be the parent span id
  // for Outcome and SpanClose these this would be the span id of the opening Onset and SpanOpen events
  // For Hibernate and Mark this would be the span under which they were emitted.
  // spanId is not set ONLY if:
  //  1. This is an Onset event
  //  2. We are not inherting any SpanContext. (e.g. this is a cross-account service binding or a new top-level invocation)
  readonly spanId?: string;
}

interface TailEvent<Event extends EventType> {
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
  spanClose?: TailEventHandler<SpanClose>;
  diagnosticChannel?: TailEventHandler<DiagnosticChannelEvent>;
  exception?: TailEventHandler<Exception>;
  log?: TailEventHandler<Log>;
  return?: TailEventHandler<Return>;
  attributes?: TailEventHandler<Attributes>;
};

type TailEventHandlerType = TailEventHandler | TailEventHandlerObject;
}
