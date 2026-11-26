// EventSource's published declaration, with the EventSourceInit its
// constructor takes. EventSource extends the EventTarget that
// events/event-target-internals provides, which TypeScript would emit as a
// variable of the module that the Workers types cannot publish, so the
// declarations are written here, and eventsource.ts checks that its class
// agrees with them. Only JSDoc reaches the Workers types, which add the
// standard comments by name.
//
// EventSource is a global at every compatibility date.

declare class EventSource extends EventTarget {
  constructor(url: string, init?: EventSourceEventSourceInit);
  close(): void;
  get url(): string;
  get withCredentials(): boolean;
  get readyState(): number;
  get onopen(): any | null;
  set onopen(value: any | null);
  get onmessage(): any | null;
  set onmessage(value: any | null);
  get onerror(): any | null;
  set onerror(value: any | null);
  static readonly CONNECTING: number;
  static readonly OPEN: number;
  static readonly CLOSED: number;
  static from(stream: ReadableStream): EventSource;
}

interface EventSourceEventSourceInit {
  withCredentials?: boolean;
  fetcher?: Fetcher;
}

export type { EventSource, EventSourceEventSourceInit };
