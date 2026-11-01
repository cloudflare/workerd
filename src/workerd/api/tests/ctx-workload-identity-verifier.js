import { WorkerEntrypoint } from 'cloudflare:workers';
export class Verifier extends WorkerEntrypoint {
  async verify(request) {
    if (this.ctx.props.testOnlyScope !== 'native')
      throw new Error('Receiver scope was lost');
    const token = request.headers.get('authorization');
    if (token === 'Bearer stalled') return await new Promise(() => {});
    if (token === 'Bearer outage') throw new Error('outage');
    if (token === 'Bearer invalid') return { status: 'invalid' };
    // Yield to exercise concurrent invocation isolation and memoization.
    await scheduler.wait(1);
    return {
      status: 'verified',
      identity: {
        kind: 'signed-request',
        signatureAgent: token,
        body: await request.text(),
        method: request.method,
        url: request.url,
      },
    };
  }
}
