// EventSource, a global at every compatibility date.
/**
 * The **`EventSource`** interface is web content's interface to server-sent events.
 *
 * [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventSource)
 */
declare class EventSource extends EventTarget {
  constructor(url: string, init?: EventSourceEventSourceInit);
  /**
   * The **`close()`** method of the EventSource interface closes the connection, if one is made, and sets the EventSource.readyState attribute to 2 (closed).
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventSource/close)
   */
  close(): void;
  /**
   * The **`url`** read-only property of the EventSource interface returns a string representing the URL of the source.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventSource/url)
   */
  get url(): string;
  /**
   * The **`withCredentials`** read-only property of the EventSource interface returns a boolean value indicating whether the EventSource object was instantiated with CORS credentials set.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventSource/withCredentials)
   */
  get withCredentials(): boolean;
  /**
   * The **`readyState`** read-only property of the EventSource interface returns a number representing the state of the connection.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventSource/readyState)
   */
  get readyState(): number;
  /* [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventSource/open_event) */
  get onopen(): any | null;
  /* [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventSource/open_event) */
  set onopen(value: any | null);
  /* [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventSource/message_event) */
  get onmessage(): any | null;
  /* [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventSource/message_event) */
  set onmessage(value: any | null);
  /* [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventSource/error_event) */
  get onerror(): any | null;
  /* [MDN Reference](https://developer.mozilla.org/docs/Web/API/EventSource/error_event) */
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
