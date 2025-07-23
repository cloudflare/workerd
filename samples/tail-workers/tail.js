import { WorkerEntrypoint } from 'cloudflare:workers';

export default class TailWorker extends WorkerEntrypoint {
  fetch() {
    console.log('Fetch event received in Tail Worker');
    return new Response('Hello from Tail Worker!');
  }

  tail(events) {
    console.log('Tail event received in Tail Worker:', events);
    console.log('tail worker', events[0].logs, events[0].event);
  }
}
