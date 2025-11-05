function shouldChaos() {
  return Math.random() < 0.10; // 10% chaos for Analytics
}

function getChaoticAnalyticsResponse() {
  const chaos = Math.random();
  if (chaos < 0.3) {
    // Data ingestion limit exceeded
    return new Response('Daily quota exceeded', { status: 429 });
  } else if (chaos < 0.5) {
    // Invalid data format
    return new Response('Invalid analytics data format', { status: 400 });
  } else if (chaos < 0.7) {
    // Service temporarily down  
    return new Response('Analytics service temporarily unavailable', { status: 503 });
  } else if (chaos < 0.9) {
    // Success but with warning
    return new Response('Data accepted with warnings', { status: 202 });
  } else {
    // Unexpected server behavior
    return new Response('<html><body>Maintenance page</body></html>', { 
      status: 200,
      headers: { 'Content-Type': 'text/html' }
    });
  }
}

export default {
  async fetch(request, env, ctx) {
    // Random chaos injection
    if (shouldChaos()) {
      return getChaoticAnalyticsResponse();
    }
    
    // Analytics Engine accepts data points via HTTP POST
    if (request.method === 'POST') {
      const body = await request.text();
      console.log('Analytics data received:', body.slice(0, 100));
      
      // Sometimes simulate processing delay or partial acceptance
      if (Math.random() < 0.05) {
        return new Response('Partial data accepted', { status: 206 });
      }
      
      return new Response('', { status: 200 });
    }
    
    return new Response('Analytics mock ready', { status: 200 });
  }
};