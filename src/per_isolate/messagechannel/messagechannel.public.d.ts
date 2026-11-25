// MessagePort's and MessageChannel's published declarations, with the
// options postMessage() takes. MessagePort extends a stand-in for
// EventTarget (see messagechannel.ts), which TypeScript would emit as a
// variable the Workers types cannot publish, so the declarations are written
// here, and messagechannel.ts checks that its classes agree with them. Only
// JSDoc reaches the Workers types, which add the standard comments by name.
//
// They are declared at every compatibility date: MessageChannel and
// MessagePort are globals under the expose_global_message_channel
// compatibility flag (the default from 2025-08-15), and node:worker_threads
// exports them at any date.

declare abstract class MessagePort extends EventTarget {
  postMessage(
    data?: any,
    options?: any[] | MessagePortPostMessageOptions
  ): void;
  close(): void;
  start(): void;
  get onmessage(): any | null;
  set onmessage(value: any | null);
}

declare class MessageChannel {
  constructor();
  readonly port1: MessagePort;
  readonly port2: MessagePort;
}

interface MessagePortPostMessageOptions {
  transfer?: any[];
}

export type { MessagePort, MessageChannel, MessagePortPostMessageOptions };
