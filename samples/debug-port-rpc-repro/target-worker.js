import { WorkerEntrypoint } from 'cloudflare:workers';

export default class TargetWorker extends WorkerEntrypoint {
  async fetch() {
    return new Response('Hello from target');
  }

  ping() {
    return 'Pong';
  }
}
