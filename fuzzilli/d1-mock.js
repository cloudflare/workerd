// Based on edgeworker D1 mock implementation with chaos
const MOCK_USER_ROWS = {
  1: { user_id: 1, name: 'Albert Ross', home: 'sky', features: 'wingspan' },
  2: { user_id: 2, name: 'Al Dente', home: 'bowl', features: 'mouthfeel' },
};

function shouldChaos() {
  return Math.random() < 0.15; // 15% chaos for D1
}

function getChaoticD1Response() {
  const chaos = Math.random();
  if (chaos < 0.25) {
    // Database locked
    return Response.json({ error: 'database is locked', success: false }, { status: 500 });
  } else if (chaos < 0.45) {
    // Connection limit exceeded  
    return Response.json({ error: 'too many connections', success: false }, { status: 503 });
  } else if (chaos < 0.65) {
    // Malformed response (missing success field)
    return Response.json({ results: [], meta: { served_by: 'chaos' } });
  } else if (chaos < 0.85) {
    // SQL constraint violation
    return Response.json({ 
      error: 'UNIQUE constraint failed: users.email', 
      success: false 
    }, { status: 400 });
  } else {
    // Completely wrong JSON structure
    return Response.json({ chaos: true, random_field: 'unexpected' });
  }
}

function mockQuery({ sql, params = [] }) {
  // Occasional chaos even in successful queries
  if (shouldChaos()) {
    if (Math.random() < 0.5) {
      throw new Error('Random SQL execution error');
    }
  }
  
  switch (sql.trim()) {
    case 'select 1;':
    case 'SELECT 1':
      return ok({ 1: 1 });
    case 'select * from users;':
      return ok(...Object.values(MOCK_USER_ROWS));
    case 'select * from users where user_id = ?;':
      return ok(MOCK_USER_ROWS[params[0]]);
    default:
      return ok(); // Empty result for unknown queries
  }
}

function ok(...results) {
  return {
    success: true,
    results,
    meta: { duration: Math.random() * 0.1, served_by: 'd1-mock' }, // Random duration
  };
}

export default {
  async fetch(request, env, ctx) {
    // Random top-level chaos
    if (shouldChaos()) {
      return getChaoticD1Response();
    }
    
    try {
      const { pathname } = new URL(request.url);
      
      if (request.method === 'POST' && (pathname.startsWith('/query') || pathname.startsWith('/execute'))) {
        const body = await request.json();
        return Response.json(
          Array.isArray(body)
            ? body.map((query) => mockQuery(query))
            : mockQuery(body)
        );
      }
      
      return new Response('D1 mock ready', { status: 200 });
    } catch (err) {
      return Response.json(
        { error: err.message, success: false },
        { status: 500 }
      );
    }
  }
};