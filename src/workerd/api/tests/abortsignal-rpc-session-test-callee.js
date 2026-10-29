// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';

// A signal kept by heldSignal() past the end of the request that received it.
let heldSignal;

function signalState(signal) {
  return { aborted: signal.aborted, reason: signal.reason };
}

export class Callee extends WorkerEntrypoint {
  async ignoresSignal(signal) {
    return 'ok';
  }

  async echoesSignal(signal) {
    return signal;
  }

  async holdsSignal(signal) {
    heldSignal = signal;
    return 'ok';
  }

  async heldSignalState() {
    return signalState(heldSignal);
  }
}

export class CalleeActor extends DurableObject {
  async actorIgnoresSignal(signal) {
    return 'ok';
  }

  // Keeps the signal after this request has returned.
  async actorHoldsSignal(signal) {
    this.signal = signal;
    return 'ok';
  }

  // Keeps the signal and listens for its abort after this method has returned.
  async actorListensForAbort(signal) {
    this.signal = signal;
    this.abortedWith = undefined;
    signal.addEventListener('abort', () => {
      this.abortedWith = signal.reason;
    });
    return 'ok';
  }

  async actorListensForOnabort(signal) {
    this.signal = signal;
    this.abortedWith = undefined;
    signal.onabort = () => {
      this.abortedWith = signal.reason;
    };
    return 'ok';
  }

  async actorHeldSignalState() {
    return { ...signalState(this.signal), abortedWith: this.abortedWith };
  }

  async actorReset() {
    this.ctx.abort(new Error('actor reset'));
  }
}

export default {};
