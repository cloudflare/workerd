function shouldChaos() {
  return Math.random() < 0.14; // 14% chaos for Queue
}

function getChaoticQueueResponse() {
  const chaos = Math.random();
  if (chaos < 0.2) {
    // Queue is full
    return new Response('Queue capacity exceeded', { status: 507 });
  } else if (chaos < 0.4) {
    // Message too large
    return new Response('Message size limit exceeded', { status: 413 });
  } else if (chaos < 0.6) {
    // Rate limit hit
    return new Response('Too many messages per second', { status: 429 });
  } else if (chaos < 0.8) {
    // Temporary network issue
    return new Response('Network timeout', { status: 504 });
  } else {
    // Partial success (some messages accepted, some rejected)
    return new Response(JSON.stringify({
      success: false,
      failed_messages: Math.floor(Math.random() * 3) + 1
    }), { 
      status: 207, // Multi-status
      headers: { 'Content-Type': 'application/json' }
    });
  }
}

export default {
  async fetch(request, env, ctx) {
    // Random chaos injection
    if (shouldChaos()) {
      return getChaoticQueueResponse();
    }
    
    const { pathname } = new URL(request.url);
    
    if (request.method === 'POST' && pathname === '/message') {
      // Single message
      const format = request.headers.get('X-Msg-Fmt') || 'v8';
      console.log('Queue message received with format:', format);
      
      // Sometimes reject based on format
      if (Math.random() < 0.03 && format === 'json') {
        return new Response('JSON format not supported', { status: 400 });
      }
      
      return new Response('', { status: 200 });
    } else if (request.method === 'POST' && pathname === '/batch') {
      // Batch messages
      const body = await request.json();
      const messageCount = body.messages?.length || 0;
      console.log('Queue batch received:', messageCount, 'messages');
      
      // Sometimes simulate partial batch acceptance
      if (Math.random() < 0.08 && messageCount > 1) {
        return new Response(JSON.stringify({
          accepted: Math.floor(messageCount * 0.7),
          rejected: Math.ceil(messageCount * 0.3),
          errors: ['Message 2 invalid format', 'Message 4 too large']
        }), {
          status: 207,
          headers: { 'Content-Type': 'application/json' }
        });
      }
      
      return new Response('', { status: 200 });
    }
    
    return new Response('Queue mock ready', { status: 200 });
  }
};