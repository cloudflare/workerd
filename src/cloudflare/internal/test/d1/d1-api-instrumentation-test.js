// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

// tailStream is going to be invoked multiple times, but we want to wait
// to run the test until all executions are done. Collect promises for
// each
let invocationPromises = [];
let spans = new Map();

export default {
  tailStream(event, env, ctx) {
    // For each "onset" event, store a promise which we will resolve when
    // we receive the equivalent "outcome" event
    let resolveFn;
    invocationPromises.push(
      new Promise((resolve, reject) => {
        resolveFn = resolve;
      })
    );

    // Accumulate the span info for easier testing
    return (event) => {
      let spanKey = `${event.invocationId}#${event.event.spanId || event.spanContext.spanId}`;
      switch (event.event.type) {
        case 'spanOpen':
          // The span ids will change between tests, but Map preserves insertion order
          spans.set(spanKey, { name: event.event.name });
          break;
        case 'attributes': {
          let span = spans.get(spanKey);
          for (let { name, value } of event.event.info) {
            span[name] = value;
          }
          spans.set(spanKey, span);
          break;
        }
        case 'spanClose': {
          let span = spans.get(spanKey);
          span['closed'] = true;
          spans.set(spanKey, span);
          break;
        }
        case 'outcome':
          resolveFn();
          break;
      }
    };
  },
};

export const test = {
  async test() {
    // Wait for all the tailStream executions to finish
    await Promise.allSettled(invocationPromises);

    // Recorded streaming tail worker events, in insertion order,
    // filtering spans not associated with KV
    let received = Array.from(spans.values()).filter(
      (span) => span.name !== 'jsRpcSession'
    );
    let failed = 0;
    let i = -1;
    let tolerance = 5n;
    try {
      assert.equal(received.length, expectedSpans.length);
      for (i = 0; i < received.length; i++) {
        compareBigIntProps(
          received[i],
          expectedSpans[i],
          ['http.response.body.size'],
          tolerance
        );
        assert.deepStrictEqual(received[i], expectedSpans[i]);
      }
    } catch (e) {
      failed++;
      if (i >= 0) {
        console.log('spans are not identical', e);
      } else {
        console.error(e);
      }
    }
    if (failed > 0) {
      throw 'D1 instrumentation test failed';
    }
  },
};

function compareBigIntProps(receivedSpan, expectedSpan, propNames, tolerance) {
  for (const propName of propNames) {
    if (Object.keys(expectedSpan).includes(propName)) {
      if (
        bitIntAbsDelta(receivedSpan[propName], expectedSpan[propName]) >
        tolerance
      ) {
        throw `${propName} attribute outside of tolerance`;
      } else {
        delete receivedSpan[propName];
        delete expectedSpan[propName];
      }
    }
  }
}

function bitIntAbsDelta(receivedBigInt, expectedBigInt) {
  const delta = BigInt(receivedBigInt) - BigInt(expectedBigInt);
  if (delta < 0) {
    return delta * -1n;
  }
  return delta;
}

const expectedSpans = [
  {
    name: 'prepare',
    query:
      ' CREATE TABLE users\n' +
      '        (\n' +
      '            user_id    INTEGER PRIMARY KEY,\n' +
      '            name       TEXT,\n' +
      '            home       TEXT,\n' +
      '            features   TEXT,\n' +
      '            land_based BOOLEAN\n' +
      '        );',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/execute?resultsFormat=NONE',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 231n,
    'http.response.status_code': 200n,
    'http.response.body.size': 187n,
    closed: true,
  },
  { name: 'prepare', query: 'SELECT * FROM users;', closed: true },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 42n,
    'http.response.status_code': 200n,
    'http.response.body.size': 257n,
    closed: true,
  },
  { name: 'prepare', query: 'SELECT * FROM users;', closed: true },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/execute?resultsFormat=NONE',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 42n,
    'http.response.status_code': 200n,
    'http.response.body.size': 188n,
    closed: true,
  },
  { name: 'prepare', query: 'DELETE FROM users;', closed: true },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/execute?resultsFormat=NONE',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 40n,
    'http.response.status_code': 200n,
    'http.response.body.size': 188n,
    closed: true,
  },
  {
    name: 'prepare',
    query:
      '\n' +
      '        INSERT INTO users (name, home, features, land_based) VALUES\n' +
      "          ('Albert Ross', 'sky', 'wingspan', false),\n" +
      "          ('Al Dente', 'bowl', 'mouthfeel', true)\n" +
      '        RETURNING *\n' +
      '    ',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 223n,
    'http.response.status_code': 200n,
    'http.response.body.size': 329n,
    closed: true,
  },
  { name: 'prepare', query: 'DELETE FROM users;', closed: true },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/execute?resultsFormat=NONE',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 40n,
    'http.response.status_code': 200n,
    'http.response.body.size': 187n,
    closed: true,
  },
  {
    name: 'prepare',
    query:
      '\n' +
      '        INSERT INTO users (name, home, features, land_based) VALUES\n' +
      "          ('Albert Ross', 'sky', 'wingspan', false),\n" +
      "          ('Al Dente', 'bowl', 'mouthfeel', true)\n" +
      '        RETURNING *\n' +
      '    ',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/execute?resultsFormat=NONE',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 223n,
    'http.response.status_code': 200n,
    'http.response.body.size': 355n,
    closed: true,
  },
  { name: 'prepare', query: 'select 1;', closed: true },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 31n,
    'http.response.status_code': 200n,
    'http.response.body.size': 217n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 31n,
    'http.response.status_code': 200n,
    'http.response.body.size': 216n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 31n,
    'http.response.status_code': 200n,
    'http.response.body.size': 216n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 31n,
    'http.response.status_code': 200n,
    'http.response.body.size': 216n,
    closed: true,
  },
  { name: 'prepare', query: 'SELECT * FROM users;', closed: true },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 42n,
    'http.response.status_code': 200n,
    'http.response.body.size': 329n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 42n,
    'http.response.status_code': 200n,
    'http.response.body.size': 329n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 42n,
    'http.response.status_code': 200n,
    'http.response.body.size': 330n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 42n,
    'http.response.status_code': 200n,
    'http.response.body.size': 329n,
    closed: true,
  },
  {
    name: 'prepare',
    query: 'SELECT * FROM users WHERE user_id = ?;',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 61n,
    'http.response.status_code': 200n,
    'http.response.body.size': 294n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 61n,
    'http.response.status_code': 200n,
    'http.response.body.size': 293n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 61n,
    'http.response.status_code': 200n,
    'http.response.body.size': 293n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 61n,
    'http.response.status_code': 200n,
    'http.response.body.size': 293n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 61n,
    'http.response.status_code': 200n,
    'http.response.body.size': 292n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 61n,
    'http.response.status_code': 200n,
    'http.response.body.size': 293n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 61n,
    'http.response.status_code': 200n,
    'http.response.body.size': 292n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 61n,
    'http.response.status_code': 200n,
    'http.response.body.size': 291n,
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 125n,
    'http.response.status_code': 200n,
    'http.response.body.size': 588n,
    closed: true,
  },
  {
    name: 'prepare',
    query: 'SELECT count(1) as count FROM users WHERE land_based = ?',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 79n,
    'http.response.status_code': 200n,
    'http.response.body.size': 221n,
    closed: true,
  },
  {
    name: 'prepare',
    query: 'SELECT count(1) as count FROM users WHERE land_based = ?',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 79n,
    'http.response.status_code': 200n,
    'http.response.body.size': 220n,
    closed: true,
  },
  {
    name: 'prepare',
    query: 'SELECT count(1) as count FROM users WHERE land_based = ?',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 79n,
    'http.response.status_code': 200n,
    'http.response.body.size': 220n,
    closed: true,
  },
  {
    name: 'prepare',
    query: 'SELECT count(1) as count FROM users WHERE land_based = ?',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 79n,
    'http.response.status_code': 200n,
    'http.response.body.size': 220n,
    closed: true,
  },
  {
    name: 'prepare',
    query: 'SELECT count(1) as count FROM users WHERE land_based = ?',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 79n,
    'http.response.status_code': 200n,
    'http.response.body.size': 220n,
    closed: true,
  },
  {
    name: 'prepare',
    query: 'CREATE TABLE abc (a INT, b INT, c INT);',
    closed: true,
  },
  {
    name: 'prepare',
    query: 'CREATE TABLE cde (c TEXT, d TEXT, e TEXT);',
    closed: true,
  },
  {
    name: 'prepare',
    query: 'INSERT INTO abc VALUES (1,2,3),(4,5,6);',
    closed: true,
  },
  {
    name: 'prepare',
    query:
      'INSERT INTO cde VALUES ("A", "B", "C"),("D","E","F"),("G","H","I");',
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 298n,
    'http.response.status_code': 200n,
    'http.response.body.size': 844n,
    closed: true,
  },
  { name: 'prepare', query: 'SELECT * FROM abc, cde;', closed: true },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 45n,
    'http.response.status_code': 200n,
    'http.response.body.size': 353n,
    closed: true,
  },
  { name: 'prepare', query: 'SELECT * FROM abc, cde;', closed: true },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 45n,
    'http.response.status_code': 200n,
    'http.response.body.size': 353n,
    closed: true,
  },
  { name: 'prepare', query: 'SELECT * FROM abc, cde;', closed: true },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 45n,
    'http.response.status_code': 200n,
    'http.response.body.size': 353n,
    closed: true,
  },
  { name: 'prepare', query: 'SELECT * FROM abc, cde;', closed: true },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 45n,
    'http.response.status_code': 200n,
    'http.response.body.size': 354n,
    closed: true,
  },
  {
    name: 'prepare',
    query: "SELECT * from cde WHERE c IN ('A','B','C','X','Y','Z')",
    closed: true,
  },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 76n,
    'http.response.status_code': 200n,
    'http.response.body.size': 235n,
    closed: true,
  },
  { name: 'prepare', query: 'DROP TABLE users;', closed: true },
  { name: 'prepare', query: 'DROP TABLE abc;', closed: true },
  { name: 'prepare', query: 'DROP TABLE cde;', closed: true },
  {
    name: 'fetch',
    'network.protocol.name': 'http',
    'network.protocol.version': 'HTTP/1.1',
    'http.request.method': 'POST',
    'url.full': 'http://d1/query?resultsFormat=ROWS_AND_COLUMNS',
    'http.request.header.content-type': 'application/json',
    'http.request.body.size': 117n,
    'http.response.status_code': 200n,
    'http.response.body.size': 633n,
    closed: true,
  },
];
