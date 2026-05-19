import { strictEqual, ok, throws } from 'node:assert';

// Regression test for AUTOVULN-CLOUDFLARE-WORKERD-319.
// When a BYOB read is partially filled and the controller is closed,
// handleMaybeClose resolves the read via v8::Promise::Resolver::Resolve().
// A malicious Object.prototype.then getter can call controller.error()
// re-entrantly, freeing the ConsumerImpl. The weak-ref liveness guard
// after handleMaybeClose must prevent use-after-free.
export const byobCloseReentrantErrorViaThenable = {
  async test() {
    let ctrl;
    const rs = new ReadableStream({
      type: 'bytes',
      start(c) {
        ctrl = c;
      },
      pull(c) {
        /* leave reads pending */
      },
    });
    const reader = rs.getReader({ mode: 'byob' });

    // Pending BYOB read with min=10. handleRead enqueues a pending read
    // with atLeast=10, filled=0.
    const p = reader.read(new Uint8Array(10), { min: 10 });
    // We don't care if the read fulfills or rejects.
    p.then(
      () => {},
      () => {}
    );

    // Enqueue 5 bytes — not enough to satisfy min=10, so the data is
    // buffered in the pending read's pullInto store.
    ctrl.enqueue(new Uint8Array(5));

    let armed = false;
    let fired = 0;
    // Record the error from the re-entrant ctrl.error() call. We can't
    // use assert.throws inside the getter because an AssertionError would
    // escape into V8's internal promise resolution and cause confusing
    // side effects.
    Object.defineProperty(Object.prototype, 'then', {
      configurable: true,
      get() {
        if (armed) {
          armed = false;
          fired++;
          // Re-entrant during handleMaybeClose's request->resolve().
          // Pre-fix, this would free the ConsumerImpl while
          // maybeDrainAndSetState still holds raw references to it.
          ctrl.error(new Error('reentrant'));
        }
        return undefined;
      },
    });

    armed = true;
    // close() → handleMaybeClose → request->resolve() → thenable check
    // → getter fires → re-entrant ctrl.error().
    throws(() => ctrl.close(), {
      message: /internal error/,
    });
    armed = false;
    delete Object.prototype.then;

    // If we got here without crashing, the liveness guard worked.
    strictEqual(fired, 1, 'thenable getter should have fired exactly once');
  },
};
