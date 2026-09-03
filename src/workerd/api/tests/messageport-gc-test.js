// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for a segfault in MessagePort.postMessage() with no
// adversarial input at all: posting to a port whose entangled peer has simply
// become garbage.
//
// The hazard is only reachable under a natural major GC. A forced GC (which is
// what --gc-stress and RequestGarbageCollectionForTesting() perform) sweeps
// atomically, so cppgc destroys the shim inside the GC and the window this
// test is aiming at never opens. So the test cannot ask for a GC at the right
// moment; it has to generate enough allocation pressure to be handed one.
//
// That makes it probabilistic, so it self-verifies rather than merely
// declining to crash: unsafe.getCondemnedWrapperCount() reports how many times
// a WeakRef promotion actually landed in the window. The loop stops as soon as
// that count is non-zero, which both proves the run was meaningful and cuts a
// ~100s test to a second or two in the common case.

import unsafe from 'workerd:unsafe';

// Both are upper bounds, not targets: the loop stops as soon as the counter proves the window
// was reached, which most runs manage within a second or two. Whichever bound is hit first
// ends the run, and the phase cap only matters on a machine fast enough to burn 40 phases
// inside the time budget.
const MAX_PHASES = 40;
const VERIFY_BUDGET_MS = 30000;
const ROUNDS_PER_PHASE = 500;
const CHANNELS_PER_ROUND = 8;
const LIVE_SENDERS = 600;
const PRESSURE_MB = 128;

let pressureMemory = null;
let pressureView = null;

// Off-heap pressure, touched continuously, to provoke natural major GCs.
function touchPressure() {
  if (pressureMemory == null) {
    pressureMemory = new WebAssembly.Memory({ initial: 16, maximum: 32768 });
    pressureMemory.grow(Math.ceil((PRESSURE_MB * 1024 * 1024) / 65536) - 16);
    pressureView = new Uint8Array(pressureMemory.buffer);
  }
  for (let i = 0; i < pressureView.length; i += 4096) {
    pressureView[i] = i & 0xff;
  }
}

// Churn the JS heap for roughly a millisecond alongside the off-heap writes.
function churn() {
  const end = Date.now() + 1;
  const keep = [];
  let sink = 0;
  while (Date.now() < end) {
    const garbage = new Array(4096);
    for (let i = 0; i < garbage.length; i++) {
      garbage[i] = { i, s: 'x' + i };
    }
    keep.push(garbage);
    if (keep.length > 32) keep.shift();
    touchPressure();
    for (let i = 0; i < 20000; i++) sink += Math.sqrt(i);
  }
  return sink;
}

// Returns true if the condemned-wrapper window was reached. Gives up at `deadline`, which is
// checked per round rather than per phase: a phase is thousands of postMessage calls and takes
// far longer than the whole budget, so a check at the phase boundary would never be reached in
// time.
async function pumpOrphanedPeers(deadline) {
  let senders = [];

  for (let round = 0; round < ROUNDS_PER_PHASE; round++) {
    for (let i = 0; i < CHANNELS_PER_ROUND; i++) {
      const channel = new MessageChannel();
      channel.port2.onmessage = () => {};
      channel.port1.postMessage({ round, i });
      // Retain only the sending half. Once `channel` goes out of scope, port2
      // is reachable solely through port1's internal weak ref.
      senders.push(channel.port1);
    }
    if (senders.length > LIVE_SENDERS) {
      senders = senders.slice(-LIVE_SENDERS);
    }

    churn();

    // Must not crash. Dropping the message is correct — the peer really is
    // gone — but resurrecting its zapped wrapper is not.
    for (const port of senders) {
      port.postMessage({ round, late: true });
    }

    if (unsafe.getCondemnedWrapperCount() > 0) return true;
    if (Date.now() > deadline) return false;

    if (round % 10 === 0) await scheduler.wait(0);
  }
  return false;
}

export const postMessageAfterPeerCollected = {
  async test() {
    // Several phases rather than one long loop: the window depends on where
    // the heap happens to be when a major GC lands, and phase boundaries
    // (a drained microtask queue plus a fresh sender pile) vary that far more
    // than simply running more rounds does.
    const deadline = Date.now() + VERIFY_BUDGET_MS;
    let verified = false;
    for (let phase = 0; phase < MAX_PHASES && !verified; phase++) {
      verified = await pumpOrphanedPeers(deadline);
      await scheduler.wait(0);
      if (Date.now() > deadline) break;
    }

    // Deliberately not an assertion. Reaching the window is a matter of luck: measured over
    // runs it usually happens within one phase, but the tail is long (occasionally nine
    // phases and two minutes), so any deadline short enough to be worth spending in CI is
    // also short enough to be flaky, and a flaky test costs more than this check is worth.
    //
    // Nothing is lost by giving up. The regression this test exists for is a segfault, and
    // detecting it does not depend on the counter — an unverified run is exactly as good at
    // catching a crash as this test was before the counter existed. What the counter buys is
    // that a passing run usually says "the hazardous state was entered and handled" instead
    // of merely "nothing crashed", and it runs faster on average.
    if (!verified) {
      console.warn(
        'messageport-gc-test: never reached the condemned-wrapper window within ' +
          `${VERIFY_BUDGET_MS}ms; this run only shows the absence of a crash. If this ` +
          'has become the common case, GC timing has probably shifted and the pressure ' +
          'loop needs revisiting rather than a longer budget.'
      );
    }
  },
};
