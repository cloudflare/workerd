import {
  strictEqual,
  ok,
  throws
} from 'node:assert';

export const acceptEventStreamTest = {
  async test(ctrl, env) {
    const eventsource = new EventSource('http://example.org/accept-event-stream',
                                        { fetcher: env.subrequest });
    strictEqual(eventsource.readyState, EventSource.CONNECTING);
    const { promise, resolve } = Promise.withResolvers();
    let opened = false;
    eventsource.onopen = () => {
      strictEqual(eventsource.readyState, EventSource.OPEN);
      opened = true;
    };
    eventsource.onmessage = (event) => {
      strictEqual(event.data, 'text/event-stream');
      strictEqual(event.origin, 'http://example.org');
      eventsource.close();
      strictEqual(eventsource.readyState, EventSource.CLOSED);
      resolve();
    };
    await promise;
    ok(opened);
  }
};

export const cacheControlEventStreamTest = {
  async test(ctrl, env) {
    const eventsource = new EventSource('http://example.org/cache-control-event-stream',
                                        { fetcher: env.subrequest });
    const { promise, resolve } = Promise.withResolvers();
    eventsource.onmessage = (event) => {
      strictEqual(event.data, 'no-cache');
      eventsource.close();
      resolve();
    };
    await promise;
  }
};

export const lastEventIdEventStreamTest = {
  async test(ctrl, env) {
    const eventsource = new EventSource('http://example.org/last-event-id',
                                        { fetcher: env.subrequest });
    const { promise, resolve } = Promise.withResolvers();
    let first = true;
    eventsource.onmessage = (event) => {
      if (first) {
        strictEqual(event.data, 'first');
        first = false;
      } else {
        strictEqual(event.data, '1');
        eventsource.close();
        resolve();
      }
    };

    await promise;
  }
};

export const eventIdPersistsTest = {
  async test(ctrl, env) {
    const eventsource = new EventSource('http://example.org/event-id-persists',
                                      { fetcher: env.subrequest });
    const { promise, resolve } = Promise.withResolvers();
    eventsource.onmessage = (event) => {
      switch (event.data) {
        case 'first':
          strictEqual(event.lastEventId, '1');
          break;
        case 'second':
          strictEqual(event.lastEventId, '1');
          break;
        case 'third':
          strictEqual(event.lastEventId, '2');
          break;
        case 'fourth':
          strictEqual(event.lastEventId, '2');
          eventsource.close();
          resolve();
          break;
        default:
          throw new Error(`Unexpected message: ${event.data}`);
      }
    };
    await promise;
  }
};

export const eventIdResetsTest = {
  async test(ctrl, env) {
    const eventsource = new EventSource('http://example.org/event-id-resets',
                                      { fetcher: env.subrequest });
    const { promise, resolve } = Promise.withResolvers();
    eventsource.onmessage = (event) => {
      switch (event.data) {
        case 'first':
          strictEqual(event.lastEventId, '1');
          break;
        case 'second':
          strictEqual(event.lastEventId, '');
          eventsource.close();
          resolve();
          break;
        default:
          throw new Error(`Unexpected message: ${event.data}`);
      }
    };
    await promise;
  }
};

export const eventIdResets2Test = {
  async test(ctrl, env) {
    const eventsource = new EventSource('http://example.org/event-id-resets-2',
                                      { fetcher: env.subrequest });
    const { promise, resolve } = Promise.withResolvers();
    eventsource.onmessage = (event) => {
      switch (event.data) {
        case 'first':
          strictEqual(event.lastEventId, '1');
          break;
        case 'second':
          strictEqual(event.lastEventId, '');
          eventsource.close();
          resolve();
          break;
        default:
          throw new Error(`Unexpected message: ${event.data}`);
      }
    };
    await promise;
  }
};

export const messageTest = {
  async test(ctrl, env) {
    const eventsource = new EventSource('http://example.org/message',
                                      { fetcher: env.subrequest });
    const { promise, resolve } = Promise.withResolvers();
    // We should get three messages...
    let count = 0;
    eventsource.onmessage = (event) => {
      switch (count++) {
        case 0: {
          strictEqual(event.data, 'one\ntwo');
          break;
        }
        case 1: {
          strictEqual(event.data, 'end');
          eventsource.close();
          resolve();
          break;
        }
      }
    };
    await promise;
  }

};

export const reconnectFailTest = {
  async test(ctrl, env) {
    const eventsource = new EventSource('http://example.org/reconnect-fail',
                                      { fetcher: env.subrequest });
    const { promise, resolve } = Promise.withResolvers();
    let count = 0;
    eventsource.onmessage = (event) => {
      switch (count++) {
        case 0: {
          strictEqual(event.data, 'opened');
          break;
        }
        case 1: {
          strictEqual(event.data, 'reconnected');
          break;
        }
      }
    };
    // Should be called four times.
    let errorCount = 0;
    eventsource.onerror = (event) => {
      if (errorCount < 3) {
        strictEqual(eventsource.readyState, EventSource.CONNECTING);
      }
      if (++errorCount === 4) {
        strictEqual(eventsource.readyState, EventSource.CLOSED);
        resolve();
      }
    };
    await promise;
  }
};

export const statusErrorTest = {
  async test(ctrl, env) {
    const eventsource = new EventSource('http://example.org/status-error',
                                      { fetcher: env.subrequest });
    const { promise, resolve } = Promise.withResolvers();
    eventsource.onopen = () => {
      throw new Error('should not be called');
    };
    eventsource.onerror = (event) => {
      strictEqual(eventsource.readyState, EventSource.CLOSED);
      resolve();
    };
    await promise;
  }
}

export const eventTest = {
  async test(ctrl, env) {
    const eventsource = new EventSource('http://example.org/event',
                                      { fetcher: env.subrequest });
    const { promise, resolve } = Promise.withResolvers();
    let count = 0;
    eventsource.ontest = (event) => {
      switch (count++) {
        case 0: {
          strictEqual(event.data, 'first');
          break;
        }
        case 1: {
          strictEqual(event.data, 'second');
          eventsource.close();
          resolve();
          break;
        }
      }
    };
    await promise;
  }
};

export const retryTest = {
  async test(ctrl, env) {
    const eventsource = new EventSource('http://example.org/retry',
                                      { fetcher: env.subrequest });
    const { promise, resolve } = Promise.withResolvers();
    let count = 0;
    eventsource.onmessage = (event) => {
      switch (count++) {
        case 0: {
          strictEqual(event.data, 'first');
          break;
        }
        case 1: {
          strictEqual(event.data, 'second');
          break;
        }
        case 2: {
          strictEqual(event.data, 'first');
          break;
        }
        case 3: {
          strictEqual(event.data, 'second');
          eventsource.close();
          resolve();
          break;
        }
      }
    };
    await promise;
  }
};

export const constructorTest = {
  test() {
    throws(() => new EventSource('not a valid url'), {
      name: 'SyntaxError',
      message: 'Cannot open an EventSource to \'not a valid url\'. The URL is invalid.'
    });

    throws(() => new EventSource(123), {
      name: 'SyntaxError',
      message: 'Cannot open an EventSource to \'123\'. The URL is invalid.'
    });

    throws(() => new EventSource('http://example.org', { withCredentials: true }), {
      name: 'NotSupportedError',
      message: 'The init.withCredentials option is not supported. It must be false or undefined.'
    });

    // Doesn't throw
    (new EventSource('http://example.org/message')).close();
    (new EventSource('http://example.org/message', { withCredentials: false })).close();
    (new EventSource('http://example.org/message', { withCredentials: undefined })).close();
  }
};

export const eventSourceFromTest = {
  async test() {
    const enc = new TextEncoder();
    const chunks = [
      'data: first\n\n',
      'data: second\n\n',
      'data: third\n\n',
    ];
    const rs = new ReadableStream({
      async pull(c) {
        await scheduler.wait(10);
        c.enqueue(enc.encode(chunks.shift()));
        if (chunks.length === 0) {
          c.close();
        }
      }
    });
    const { promise, resolve } = Promise.withResolvers();
    const eventsource = EventSource.from(rs);
    // Should happen three times
    let count = 0;
    eventsource.onmessage = (event) => {
      switch (count++) {
        case 0: {
          strictEqual(event.data, 'first');
          break;
        }
        case 1: {
          strictEqual(event.data, 'second');
          break;
        }
        case 2: {
          strictEqual(event.data, 'third');
          eventsource.close();
          resolve();
          break;
        }
      }
    };
    await promise;
  }
};

export const eventSourceFromTestWithBOM = {
  async test() {
    const enc = new TextEncoder();
    // The first chunk is going to include the UTF-8 BOM, which should
    // be ignored and filtered out by the parser.
    const chunks = [
      '\uFEFFdata: first\n\n',
      'data: second\n\n',
      'data: third\n\n',
    ];
    const rs = new ReadableStream({
      async pull(c) {
        await scheduler.wait(10);
        c.enqueue(enc.encode(chunks.shift()));
        if (chunks.length === 0) {
          c.close();
        }
      }
    });
    const { promise, resolve } = Promise.withResolvers();
    const eventsource = EventSource.from(rs);
    // Should happen three times
    let count = 0;
    eventsource.onmessage = (event) => {
      switch (count++) {
        case 0: {
          strictEqual(event.data, 'first');
          break;
        }
        case 1: {
          strictEqual(event.data, 'second');
          break;
        }
        case 2: {
          strictEqual(event.data, 'third');
          eventsource.close();
          resolve();
          break;
        }
      }
    };
    await promise;
  }
};

export const prototypePropertyTest = {
  test() {
    strictEqual(EventSource.prototype.constructor, EventSource);
    strictEqual(EventSource.prototype.CLOSED, 2);
    strictEqual(EventSource.prototype.CONNECTING, 0);
    strictEqual(EventSource.prototype.OPEN, 1);
    ok('onopen' in EventSource.prototype);
    ok('onmessage' in EventSource.prototype);
    ok('onerror' in EventSource.prototype);
    ok('close' in EventSource.prototype);
    ok('readyState' in EventSource.prototype);
    ok('url' in EventSource.prototype);
    ok('withCredentials' in EventSource.prototype);
  }
};

export const dispoable = {
  test() {
    // EventSource is not defined by the spec as being disposable using ERM, but
    // it makes sense to do so. The dispose operation simply defers to close()
    const rs = new ReadableStream();
    const eventsource = EventSource.from(rs);
    strictEqual(eventsource.readyState, EventSource.OPEN);
    eventsource[Symbol.dispose]();
    strictEqual(eventsource.readyState, EventSource.CLOSED);
  }
};

// ======================================================================================

const handlers = {
  '/accept-event-stream': acceptEventStream,
  '/cache-control-event-stream': cacheControlEventStream,
  '/last-event-id': lastEventId,
  '/event-id-persists': eventIdPersists,
  '/event-id-resets': eventIdResets,
  '/event-id-resets-2': eventIdResets2,
  '/message': message,
  '/reconnect-fail': reconnectFail,
  '/status-error': statusError,
  '/event': event,
  '/retry': retry,
};

async function acceptEventStream(request) {
  return new Response(`data: ${request.headers.get('accept')}\n\n`, {
    headers: {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
    },
  });
}

async function cacheControlEventStream(request) {
  return new Response(`data: ${request.headers.get('cache-control')}\n\n`, {
    headers: {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
    },
  });
}

async function lastEventId(request) {
  const lastEventId = request.headers.get('last-event-id');
  if (lastEventId == null) {
    return new Response('id: 1\ndata: first\n\n', {
      headers: {
        'Content-Type': 'text/event-stream',
        'Cache-Control': 'no-cache'
      },
    });
  } else {
    return new Response(`data: ${lastEventId}\n\n`, {
      headers: {
        'Content-Type': 'text/event-stream',
        'Cache-Control': 'no-cache'
      },
    });
  }
}

async function eventIdPersists(request) {
  return new Response(
    'id: 1\ndata: first\n\n' +
    'data: second\n\n' +
    'id: 2\ndata: third\n\n' +
    'data: fourth\n\n', {
    headers: {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache'
    },
  });
}

async function eventIdResets(request) {
  return new Response(
    'id: 1\ndata: first\n\n' +
    'id: \ndata: second\n\n', {
    headers: {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache'
    },
  });
}

async function eventIdResets2(request) {
  return new Response(
    'id: 1\ndata: first\n\n' +
    'id\ndata: second\n\n', {
    headers: {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache'
    },
  });
}

async function message(request) {
  return new Response(
    'data: one\n' +
    'data: two\n\n' +
    ': comment' +
    'falsefield:msg\n\n' +
    'falsefield:msg\n' +
    'Data: data\n\n' +
    'data\n\n' +
    'data:end\n\n', {
    headers: {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache'
    },
  });

}

let reconnectTestCount = 0;
async function reconnectFail(request) {
  switch (reconnectTestCount++) {
    case 0: {
      return new Response(
        'data: opened\n\n', {
        headers: {
          'Content-Type': 'text/event-stream',
          'Cache-Control': 'no-cache'
        },
      });
    }
    case 1: {
      return new Response(
        'data: reconnected\n\n', {
        headers: {
          'Content-Type': 'text/event-stream',
          'Cache-Control': 'no-cache'
        },
      });
    }
    case 2:
      // Fall-through
    case 3: {
      return new Response(
        null, {
        headers: {
          'Content-Type': 'text/event-stream',
          'Cache-Control': 'no-cache'
        },
        status: 204
      });

    }
  }
}

async function statusError(request) {
  return new Response(null, {
    status: 500
  });
}

async function event(request) {
  return new Response(
    'event: test\n' +
    'data: first\n\n' +
    'event: test\n' +
    'data: second\n\n', {
    headers: {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache'
    },
  });
}

async function retry(request) {
  return new Response(
    'retry: 3000\n\n' +
    'data: first\n\n' +
    'data: second\n\n', {
    headers: {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache'
    },
  });
}

export default {
  async fetch(request) {
    const url = new URL(request.url, 'http://example.org/');
    const handler = handlers[url.pathname];
    if (handler === undefined) {
      throw new Error('Not found');
    }
    return await handler(request);
  }
};

