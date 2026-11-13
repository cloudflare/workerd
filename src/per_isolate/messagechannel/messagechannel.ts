'use strict';

// MessageChannel and MessagePort, an approximation of the HTML standard's
// that departs from it as follows:
//
// - Transfer lists are not supported. postMessage() converts and reads its
//   options but throws for a non-empty list, and a port cannot be
//   serialized, so it cannot be sent anywhere.
// - A message that cannot be serialized makes postMessage() throw, rather
//   than making the receiving port dispatch 'messageerror'. Instead, a
//   throwing 'message' listener, whose exception is reported per spec,
//   makes the port additionally dispatch a 'messageerror' event carrying
//   the exception as its data, which the spec does not do.
// - The port message queue follows the port's 'message' listeners, as in
//   Node.js. While the port has any, whether assigned to onmessage or added
//   with addEventListener() (which per spec does not enable the queue),
//   messages are delivered; once the last is removed, they are queued until
//   another arrives or start() is called. Queued messages are held as JS
//   values, with no memory accounting of their own, and do not keep their
//   port from being collected.
// - close() fires 'close' on both ports, as Node.js does; the spec removed
//   that event. A port whose peer is collected closes without one, when it
//   next looks for the peer (posting, closing, starting, or a change in its
//   'message' listeners), at a time that depends on the collector.
// - Unlike in Node.js, an open port keeps nothing running, though closing
//   ports when done with them is still best.
//
// Some webmessaging WPTs fail because of these, and many more need windows,
// workers or BroadcastChannel; src/wpt/webmessaging-test.ts records which
// run, and why the others do not pass.
//
// Otherwise the classes look as JSG's do (see the end of this file), except
// that MessagePort's constructor inherits from EventTarget's, as WebIDL
// requires, where a JSG constructor's [[Prototype]] is Function.prototype;
// both constructors' `prototype` is non-writable even without
// immutable_api_prototypes; calling either without `new` throws the
// language's TypeError rather than JSG's; and an array passed as
// postMessage()'s options is recognized by Array.isArray(), which sees
// through a proxy, where JSG's conversion does not.
//
// src/tests/events/messagechannel/ specifies the behavior, and its AGENTS.md
// indexes which test pins what.
//
// HOW IT FITS TOGETHER
//
// A port is a real EventTarget, extending the C++ one through
// events/event-target-internals. Its listener-count hook on 'message' starts
// the port when the first listener arrives and returns it to queueing when
// the last leaves. Messages are cloned with utils.cloneMessageData() and
// dispatched from reactions scheduled with utils.queueReaction(), so a throw
// that escapes a delivery is an unhandled rejection. Each port is branded
// "workerd:MessagePort" so that C++ accepts it as a MessageEvent's source or
// one of its ports.
//
// The peer is held weakly, so dropping one port of a pair lets it be
// collected; the MessageChannel holds both strongly.
//
// main.ts loads this module on first use, so its top level runs after user
// code and reads nothing that code can change (see EventTargetHeritage).

import type {
  EventTargetInternals,
  ExceptionPolicy,
} from '../events/event-target-internals';
import type { RuntimeClassHelpers } from '../runtime-class';

const {
  ArrayIsArray,
  ArrayPrototypePush,
  Error,
  EventTargetPrototype,
  ObjectDefineProperties,
  ObjectGetOwnPropertyDescriptor,
  ObjectSetPrototypeOf,
  Symbol,
  TypeError,
  WeakRef,
  WeakRefPrototypeDeref,
} = primordials;

const { cloneMessageData, getPrivateSymbol, queueReaction } = utils;

const {
  EventTarget,
  createTrustedEvent,
  createTrustedMessageEvent,
  dispatchEvent,
  effectiveExceptionPolicy,
  getEventHandlerAttribute,
  setEventHandlerAttribute,
  setListenerCountHook,
} = require('events/event-target-internals') as EventTargetInternals;
const { markAsRuntimeClass, setNameAndLength } =
  require('runtime-class') as RuntimeClassHelpers;

// JSG's messages, so that misuse reads as it does with the runtime's C++
// classes.
const kIllegalInvocation =
  'Illegal invocation: function called with incorrect `this` reference. ' +
  'See https://developers.cloudflare.com/workers/observability/errors/' +
  '#illegal-invocation-errors for details.';
const kBadOptions =
  "Failed to execute 'postMessage' on 'MessagePort': parameter 2 is not " +
  "of type 'Array or PostMessageOptions'.";
const kBadTransfer =
  "Incorrect type for the 'transfer' field on 'PostMessageOptions': the " +
  "provided value is not of type 'Array'.";

// The brand C++ recognizes ports by (messageevent::MessagePort::BRAND).
const kMessagePortBrand = getPrivateSymbol('workerd:MessagePort');

// Lets MessageChannel construct the ports that `new MessagePort()` refuses.
const kConstructPort: symbol = Symbol('kConstructPort');

// Called with each element a transfer list is read for; see below.
function ignoreElement(_element: unknown): void {}

// The number of elements in a transfer list, converted as JSG converts a
// sequence<any>: every element is read, so a getter on one runs.
function transferListLength(list: unknown[]): number {
  const length = list.length;
  for (let i = 0; i < length; i++) {
    ignoreElement(list[i]);
  }
  return length;
}

// Converts postMessage()'s second argument, an optional
// `(sequence<any> or PostMessageOptions)`, as JSG converts such a union, and
// reports whether it names anything to transfer.
function hasTransfer(options: unknown): boolean {
  if (options === undefined || options === null) return false;
  if (ArrayIsArray(options)) {
    return transferListLength(options as unknown[]) > 0;
  }
  if (typeof options !== 'object' && typeof options !== 'function') {
    throw new TypeError(kBadOptions);
  }
  const transfer = (options as { transfer?: unknown }).transfer;
  if (transfer === undefined) return false;
  if (!ArrayIsArray(transfer)) throw new TypeError(kBadTransfer);
  return transferListLength(transfer as unknown[]) > 0;
}

type PortState = 'pending' | 'started' | 'closed';

// Binds two new ports so that each delivers what is posted to the other.
let entangle: (port1: MessagePort, port2: MessagePort) => void;

// The brand check of every MessagePort method: any receiver that is not a
// port, primitives and undefined included, is an illegal invocation.
let requirePort: (receiver: unknown) => void;

// This module loads on first use, when user code may already have replaced
// EventTarget.prototype, which `extends EventTarget` would read. MessagePort
// is defined over this stand-in instead, whose `prototype` is the one the
// bootstrap captured, and then inherits from EventTarget itself, which is
// what super() calls: it looks the parent constructor up when it runs.
function EventTargetHeritage(): void {}
EventTargetHeritage.prototype = EventTargetPrototype;

class MessagePort extends (EventTargetHeritage as unknown as typeof EventTarget) {
  #state: PortState = 'pending';
  #pending: unknown[] = [];
  #peer: WeakRef<MessagePort> | undefined = undefined;

  static {
    entangle = (port1, port2): void => {
      port1.#peer = new WeakRef(port2);
      port2.#peer = new WeakRef(port1);
    };
    requirePort = (receiver): void => {
      if (
        typeof receiver !== 'object' ||
        receiver === null ||
        !(#state in receiver)
      ) {
        throw new TypeError(kIllegalInvocation);
      }
    };
  }

  constructor(token?: symbol) {
    if (token !== kConstructPort) throw new TypeError('Illegal constructor');
    super();
    (this as unknown as Record<symbol, unknown>)[kMessagePortBrand] = true;
    setListenerCountHook(this, 'message', (_type, count) => {
      this.#listenerCountChanged(count);
    });
  }

  postMessage(data?: unknown, options?: unknown): void {
    requirePort(this);
    if (hasTransfer(options)) throw new Error('Transfer list is not supported');
    // Serialized first, even with no live peer: a value that cannot be
    // cloned throws whether or not anything would receive it.
    const clone = cloneMessageData(data);
    const peer = this.#livePeer();
    if (peer !== undefined) peer.#deliver(clone);
  }

  close(): void {
    requirePort(this);
    this.#close();
  }

  start(): void {
    requirePort(this);
    this.#livePeer();
    this.#start();
  }

  get onmessage(): unknown {
    requirePort(this);
    return getEventHandlerAttribute(this, 'message');
  }

  set onmessage(value: unknown) {
    requirePort(this);
    // The attribute's trampoline flows through the listener-count hook,
    // which starts and stops the port.
    setEventHandlerAttribute(this, 'message', value);
  }

  // The peer, or undefined once this port is closed. A peer that has been
  // collected closes this port, without a 'close' event.
  #livePeer(): MessagePort | undefined {
    if (this.#peer === undefined) return undefined;
    const peer = WeakRefPrototypeDeref(this.#peer) as MessagePort | undefined;
    if (peer === undefined) this.#closeQuietly();
    return peer;
  }

  #closeQuietly(): void {
    this.#state = 'closed';
    this.#pending = [];
    this.#peer = undefined;
  }

  #close(): void {
    const peer = this.#livePeer();
    if (this.#state === 'closed') return;
    this.#closeQuietly();
    // The peer's 'close' event fires first; a throw from its listeners ends
    // this close().
    if (peer !== undefined) peer.#close();
    dispatchEvent(this, createTrustedEvent('close'), 'propagate');
  }

  #start(): void {
    if (this.#state !== 'pending') return;
    const list = this.#pending;
    this.#pending = [];
    this.#state = 'started';
    // Deferred, as Node.js defers it, and delivered in one reaction.
    queueReaction(() => {
      for (let i = 0; i < list.length; i++) {
        this.#dispatchMessage(list[i]);
      }
    });
  }

  #listenerCountChanged(count: number): void {
    this.#livePeer();
    if (this.#state === 'pending') {
      if (count > 0) this.#start();
    } else if (this.#state === 'started') {
      if (count === 0) this.#state = 'pending';
    }
    // Closed is terminal: listener changes never restart the port.
  }

  #deliver(value: unknown): void {
    if (this.#state === 'pending') {
      ArrayPrototypePush(this.#pending, value);
    } else if (this.#state === 'started') {
      queueReaction(() => {
        this.#dispatchMessage(value);
      });
    }
    // A closed port drops the message.
  }

  #dispatchMessage(value: unknown): void {
    const policy: ExceptionPolicy = effectiveExceptionPolicy('report');
    if (policy === 'propagate') {
      // Without spec_compliant_dispatch_exceptions: the first throwing
      // listener ends the dispatch, and its exception is dispatched as the
      // data of a second 'message' event, whose own throw escapes.
      let threw = false;
      let exception: unknown;
      try {
        dispatchEvent(
          this,
          createTrustedMessageEvent('message', value, this),
          'propagate'
        );
      } catch (e) {
        threw = true;
        exception = e;
      }
      if (threw) {
        dispatchEvent(
          this,
          createTrustedMessageEvent('message', exception, this),
          'propagate'
        );
      }
      return;
    }

    // Every 'message' listener runs, each exception is reported, and the
    // first is also dispatched as a 'messageerror' event's data.
    const outcome = dispatchEvent(
      this,
      createTrustedMessageEvent('message', value, this),
      'report'
    );
    if (outcome !== undefined) {
      dispatchEvent(
        this,
        createTrustedMessageEvent('messageerror', outcome.exception, this),
        'report'
      );
    }
  }
}
ObjectSetPrototypeOf(MessagePort, EventTarget);

class MessageChannel {
  constructor() {
    const port1 = new MessagePort(kConstructPort);
    const port2 = new MessagePort(kConstructPort);
    entangle(port1, port2);
    // As JSG's lazy instance properties are, including their order.
    ObjectDefineProperties(this, {
      __proto__: null,
      port2: {
        __proto__: null,
        value: port2,
        writable: false,
        enumerable: true,
        configurable: true,
      },
      port1: {
        __proto__: null,
        value: port1,
        writable: false,
        enumerable: true,
        configurable: true,
      },
    });
  }
}

// ---------------------------------------------------------------------------
// The rest makes the classes look as JSG's do.

const kSpecCompliantPropertyAttributes =
  compatFlags['spec_compliant_property_attributes'] === true;

// JSG methods are enumerable, and their length counts no optional
// parameters.
setNameAndLength(MessagePort.prototype.postMessage, 'postMessage', 0);
ObjectDefineProperties(MessagePort.prototype, {
  __proto__: null,
  postMessage: { __proto__: null, enumerable: true },
  close: { __proto__: null, enumerable: true },
  start: { __proto__: null, enumerable: true },
});

// A JSG accessor is enumerable, its functions anonymous and of length 0,
// unless spec_compliant_property_attributes gives them their WebIDL names
// and lengths.
const onmessage = ObjectGetOwnPropertyDescriptor(
  MessagePort.prototype,
  'onmessage'
) as PropertyDescriptor;
setNameAndLength(
  onmessage.get,
  kSpecCompliantPropertyAttributes ? 'get onmessage' : '',
  0
);
setNameAndLength(
  onmessage.set,
  kSpecCompliantPropertyAttributes ? 'set onmessage' : '',
  kSpecCompliantPropertyAttributes ? 1 : 0
);
ObjectDefineProperties(MessagePort.prototype, {
  __proto__: null,
  onmessage: { __proto__: null, enumerable: true },
});

setNameAndLength(MessagePort, 'MessagePort', 0);
markAsRuntimeClass(MessagePort.prototype, 'MessagePort');
markAsRuntimeClass(MessageChannel.prototype, 'MessageChannel');

export type MessageChannelExports = {
  MessageChannel: typeof MessageChannel;
  MessagePort: typeof MessagePort;
};

module.exports = {
  MessageChannel,
  MessagePort,
};
