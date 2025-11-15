// Chaos helpers
function shouldChaos() {
  return Math.random() < 0.12; // 12% chaos
}

function getChaoticResponse() {
  const chaos = Math.random();
  if (chaos < 0.3) {
    // Rate limited
    return new Response('Rate limited', { status: 429 });
  } else if (chaos < 0.5) {
    // Service unavailable
    return new Response('Service temporarily unavailable', { status: 503 });
  } else if (chaos < 0.7) {
    // Malformed JSON
    return new Response('{"broken": json}', { 
      status: 200,
      headers: { 'Content-Type': 'application/json' }
    });
  } else if (chaos < 0.9) {
    // Timeout simulation (just return very slow)
    return new Response('Request timeout', { status: 408 });
  } else {
    // Completely wrong content type
    return new Response('<html>This is not JSON</html>', {
      status: 200,
      headers: { 'Content-Type': 'text/html' }
    });
  }
}

export default {
  async fetch(request, env, ctx) {
    // Random chaos injection
    if (shouldChaos()) {
      return getChaoticResponse();
    }
    
    const { pathname } = new URL(request.url);
    const method = request.method;
    
    if (method === 'GET' && pathname.startsWith('/values/')) {
      // GET /values/{key}
      const key = pathname.slice(8);
      if (key === 'test-key') {
        return new Response('test-value', {
          headers: { 'Content-Type': 'text/plain' }
        });
      }
      return new Response(null, { status: 404 });
    } else if (method === 'PUT' && pathname.startsWith('/values/')) {
      // PUT /values/{key}
      return new Response(null, { status: 200 });
    } else if (method === 'GET' && pathname === '/keys') {
      // LIST operation
      return new Response(JSON.stringify({
        keys: [{ name: 'test-key' }],
        list_complete: true
      }), {
        headers: { 'Content-Type': 'application/json' }
      });
    }
    
    return new Response('KV mock ready', { status: 200 });
  }
};