import { WorkerEntrypoint } from 'cloudflare:workers';
export class Verifier extends WorkerEntrypoint {
  async verify(request) {
    const token = request.headers.get('authorization');
    if (token === 'Bearer outage') throw new Error('outage');
    if (token === 'Bearer invalid') return { status: 'invalid' };
    // Yield to exercise concurrent invocation isolation and memoization.
    await scheduler.wait(1);
    return {
      status: 'verified',
      identity: {
        kind: 'signed-request',
        id: token,
        body: await request.text(),
        method: request.method,
        url: request.url,
      },
    };
  }
}
