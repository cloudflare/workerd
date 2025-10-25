function shouldChaos() {
  return Math.random() < 0.13; // 13% chaos for R2
}

function getChaoticR2Response() {
  const chaos = Math.random();
  if (chaos < 0.2) {
    // Access denied
    return new Response('<?xml version="1.0" encoding="UTF-8"?><Error><Code>AccessDenied</Code><Message>Access Denied</Message></Error>', 
      { status: 403, headers: { 'Content-Type': 'application/xml' } });
  } else if (chaos < 0.4) {
    // Object not found  
    return new Response('<?xml version="1.0" encoding="UTF-8"?><Error><Code>NoSuchKey</Code></Error>', 
      { status: 404, headers: { 'Content-Type': 'application/xml' } });
  } else if (chaos < 0.6) {
    // Bandwidth limit exceeded
    return new Response('Rate limit exceeded', { status: 429 });
  } else if (chaos < 0.8) {
    // Corrupted metadata
    return new Response('corrupted object content', {
      headers: {
        'Content-Type': 'text/plain',
        'CF-R2-Metadata-Size': 'not-a-number', // Invalid metadata
        'ETag': 'invalid-etag-format'
      }
    });
  } else {
    // Wrong content type but valid data
    return new Response(JSON.stringify({ not: 'xml', but: 'json' }), {
      headers: { 'Content-Type': 'application/xml' } // Lying about content type
    });
  }
}

export default {
  async fetch(request, env, ctx) {
    // Random chaos injection
    if (shouldChaos()) {
      return getChaoticR2Response();
    }
    
    const { pathname } = new URL(request.url);
    const method = request.method;
    
    if (method === 'GET' && pathname !== '/') {
      // GET object
      return new Response('test-object-content', {
        headers: {
          'Content-Type': 'text/plain',
          'CF-R2-Metadata-Size': '18'
        }
      });
    } else if (method === 'PUT') {
      // PUT object  
      return new Response(JSON.stringify({
        etag: 'mock-etag-' + Math.random().toString(36).substr(2, 9)
      }), {
        headers: { 'Content-Type': 'application/json' }
      });
    } else if (method === 'GET' && pathname === '/') {
      // LIST objects (sometimes return inconsistent results)
      const objects = [{ key: 'test-object', metadata: { size: 18 } }];
      if (Math.random() < 0.1) {
        // Sometimes add random objects to list
        objects.push({ key: 'mystery-object', metadata: { size: Math.floor(Math.random() * 1000) } });
      }
      
      return new Response(JSON.stringify({
        keys: objects,
        is_truncated: Math.random() < 0.05 // Sometimes claim truncation
      }), {
        headers: { 'Content-Type': 'application/json' }
      });
    }
    
    return new Response('R2 mock ready', { status: 200 });
  }
};