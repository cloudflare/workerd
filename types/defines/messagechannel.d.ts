// MessageChannel and MessagePort, declared at every compatibility date. They
// are globals under the expose_global_message_channel compatibility flag (the
// default from 2025-08-15); node:worker_threads exports them at any date.
/**
 * The **`MessagePort`** interface of the Channel Messaging API represents one of the two ports of a MessageChannel, allowing messages to be sent from one port and listening out for them arriving at the other.
 *
 * [MDN Reference](https://developer.mozilla.org/docs/Web/API/MessagePort)
 */
declare abstract class MessagePort extends EventTarget {
  /**
   * The **`postMessage()`** method of the MessagePort interface sends a message from the port, and optionally, transfers ownership of objects to other browsing contexts.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/MessagePort/postMessage)
   */
  postMessage(
    data?: any,
    options?: any[] | MessagePortPostMessageOptions
  ): void;
  /**
   * The **`close()`** method of the MessagePort interface disconnects the port, so it is no longer active. This stops the flow of messages to that port.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/MessagePort/close)
   */
  close(): void;
  /**
   * The **`start()`** method of the MessagePort interface starts the sending of messages queued on the port. This method is only needed when using EventTarget.addEventListener; it is implied when using onmessage.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/MessagePort/start)
   */
  start(): void;
  get onmessage(): any | null;
  set onmessage(value: any | null);
}
/**
 * The **`MessageChannel`** interface of the Channel Messaging API allows us to create a new message channel and send data through it via its two MessagePort properties.
 *
 * [MDN Reference](https://developer.mozilla.org/docs/Web/API/MessageChannel)
 */
declare class MessageChannel {
  constructor();
  /**
   * The **`port1`** read-only property of the MessageChannel interface returns the first port of the message channel — the port attached to the context that originated the channel.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/MessageChannel/port1)
   */
  readonly port1: MessagePort;
  /**
   * The **`port2`** read-only property of the MessageChannel interface returns the second port of the message channel — the port attached to the context at the other end of the channel, which the message is initially sent to.
   *
   * [MDN Reference](https://developer.mozilla.org/docs/Web/API/MessageChannel/port2)
   */
  readonly port2: MessagePort;
}
interface MessagePortPostMessageOptions {
  transfer?: any[];
}
