declare namespace TailStream {

interface Header {
  readonly name: string;
  readonly value: string;
}

interface FetchEventInfo {
  readonly type: "fetch";
  readonly method: string;
  readonly url: string;
  readonly cfJson: string;
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

interface Resume {
  readonly type: "resume";
  readonly attachment?: any;
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

interface Trigger {
  readonly traceId: string;
  readonly invocationId: string;
  readonly spanId: string;
}

interface Onset {
  readonly type: "onset";
  readonly dispatchNamespace?: string;
  readonly entrypoint?: string;
  readonly scriptName?: string;
  readonly scriptTags?: string[];
  readonly scriptVersion?: ScriptVersion;
  readonly trigger?: Trigger;
  readonly info: FetchEventInfo | JsRpcEventInfo | ScheduledEventInfo |
                 AlarmEventInfo | QueueEventInfo | EmailEventInfo |
                 TraceEventInfo | HibernatableWebSocketEventInfo |
                 Resume | CustomEventInfo;
}

interface Outcome {
  readonly type: "outcome";
  readonly outcome: EventOutcome;
  readonly cpuTime: number;
  readonly wallTime: number;
}

interface Hibernate {
  readonly type: "hibernate";
}

interface SpanOpen {
  readonly type: "spanOpen";
  readonly op?: string;
  readonly info?: FetchEventInfo | JsRpcEventInfo | Attribute[];
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
  readonly message: string;
}

interface Return {
  readonly type: "return";
  readonly info?: FetchResponseInfo | Attribute[];
}

interface Link {
  readonly type: "link";
  readonly label?: string;
  readonly traceId: string;
  readonly invocationId: string;
  readonly spanId: string;
}

interface Attribute {
  readonly type: "attribute";
  readonly name: string;
  readonly value: string | string[] | boolean | boolean[] | number | number[];
}

type Mark = DiagnosticChannelEvent | Exception | Log | Return | Link | Attribute[];

interface TailEvent {
  readonly traceId: string;
  readonly invocationId: string;
  readonly spanId: string;
  readonly timestamp: Date;
  readonly sequence: number;
  readonly event: Onset | Outcome | Hibernate | SpanOpen | SpanClose | Mark;
}

type TailEventHandler = (event: TailEvent) => void | Promise<void>;
type TailEventHandlerName = "onset" | "outcome" | "hibernate" | "spanOpen" | "spanClose" |
                            "diagnosticChannel" | "exception" | "log" | "return" | "link" | "attribute";
type TailEventHandlerObject = Record<TailEventHandlerName, TailEventHandler>;
type TailEventHandlerType = TailEventHandler | TailEventHandlerObject;

}
