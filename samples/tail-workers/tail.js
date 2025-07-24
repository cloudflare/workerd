import { WorkerEntrypoint } from 'cloudflare:workers';

export default class TailWorker extends WorkerEntrypoint {
  async fetch(request) {
    console.log('Fetch event received in Tail Worker');
    
    // Echo server: respond with status 200 and the iter URL parameter
    const url = new URL(request.url);
    const iter = url.searchParams.get('iter') || 'no-iter-param';
    
    return new Response(iter, { 
      status: 200,
      headers: { 'Content-Type': 'text/plain' }
    });
  }

  tail(events) {
    // Tail handler for receiving tail events via RPC
    // Just silently process them for benchmarking
    return;
  }
}
