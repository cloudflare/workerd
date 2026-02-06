import { ok } from 'node:assert';

// Allow up to 10ms of jitter because the precise_timers compat flag
// can introduce +/-3ms of variance in timer resolution, and coverage
// builds add additional overhead.
const JITTER = 10;

export default {
  async test() {
    const t0 = Date.now();
    await new Promise((accept) => setTimeout(accept, 100));
    const t1 = Date.now();
    ok(t1 - t0 >= 100 - JITTER, `Received a difference of ${t1 - t0}`);
    await new Promise((accept) => setTimeout(accept, 100));
    const t2 = Date.now();
    ok(t2 - t1 >= 100 - JITTER, `Received a difference of ${t2 - t1}`);
  },
};
