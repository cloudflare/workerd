import { strictEqual } from 'node:assert';

export const temporalIsAvailable = {
  test() {
    const start = Temporal.PlainDate.from('2025-12-02');
    const end = Temporal.PlainDate.from('2026-01-05');
    const duration = start.until(end);
    strictEqual(duration.toString(), 'P34D');
  },
};
