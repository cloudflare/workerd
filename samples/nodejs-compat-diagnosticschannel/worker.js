import {
  subscribe,
  channel,
  tracingChannel,
  Channel,
} from "node:diagnostics_channel";
import { doSomething } from "library";

import { AsyncLocalStorage } from "node:async_hooks";

// The library will send a diagnostic message when the doSomething
// method is called. We listen for that event here. When using
// Tail Workers, the event will also be included in the tail events.
subscribe('test', (message) => {
  console.log('Diagnostic message received:', message);
});

const als1 = new AsyncLocalStorage();
const als2 = new AsyncLocalStorage();
const c = channel('bar');
c.bindStore(als1);
c.bindStore(als2, (data) => {return { data }});

const tc = tracingChannel('foo');
tc.start.bindStore(als1);
tc.start.subscribe(() => console.log('start', als1.getStore()));
tc.end.subscribe(() => console.log('end'));
tc.asyncStart.subscribe(() => console.log('async start'));
tc.asyncEnd.subscribe(() => console.log('async end'));

export default {
  async fetch(request) {
    doSomething();

    console.log(c.runStores(1, (...args) => {
      console.log(this, ...args, als1.getStore(), als2.getStore());
      return 1;
    }, {}, 1, 2, 3));

    await tc.tracePromise(async function() {
      console.log('....', this);
    }, {a:1});

    return new Response("ok");
  }
};
