// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

// Test helpers, since I want everything to run in sequence but I don't
// want to lose context about which assertion failed.
const test = (fn, getDB = getDBFromEnv) => ({
  async test(ctr, env) {
    await fn(getDB(env), env.d1MockFetcher);
  },
});

function getDBFromEnv(env) {
  return env.d1;
}

// Recurse through nested objects/arrays looking for 'anything' and deleting that
// key/value from both objects. Gives us a way to get expect.toMatchObject behaviour
// with only deepEqual
const anything = Symbol('anything');
const deleteAnything = (expected, actual) => {
  Object.entries(expected).forEach(([k, v]) => {
    if (v === anything) {
      delete actual[k];
      delete expected[k];
    } else if (typeof v === 'object' && typeof actual[k] === 'object') {
      deleteAnything(expected[k], actual[k]);
    }
  });
};

const itShould = async (description, ...assertions) => {
  if (assertions.length % 2 !== 0)
    throw new Error('itShould takes pairs of cb, expected args');

  try {
    for (let i = 0; i < assertions.length; i += 2) {
      const cb = assertions[i];
      const expected = assertions[i + 1];
      const actual = await cb();
      deleteAnything(expected, actual);
      try {
        assert.deepEqual(actual, expected);
      } catch (e) {
        console.log(actual);
        throw e;
      }
    }
  } catch (e) {
    throw new Error(`TEST ERROR!\n❌ Failed to ${description}\n${e.message}`);
  }
};

// Make it easy to specify only a the meta properties we're interested in
const meta = (values) => ({
  duration: anything,
  served_by: anything,
  changes: anything,
  last_row_id: anything,
  changed_db: anything,
  size_after: anything,
  rows_read: anything,
  rows_written: anything,
  ...values,
});

export const test_d1_api_happy_path = test(
  testD1ApiQueriesHappyPath,
  getDBFromEnv
);

export const test_d1_api_happy_path_withsessions_default = test(
  testD1ApiQueriesHappyPath,
  (env) => getDBFromEnv(env).withSession()
);

export const test_d1_api_happy_path_withsessions_first_unconstrained = test(
  testD1ApiQueriesHappyPath,
  (env) => getDBFromEnv(env).withSession('first-unconstrained')
);

export const test_d1_api_happy_path_withsessions_first_primary = test(
  testD1ApiQueriesHappyPath,
  (env) => getDBFromEnv(env).withSession('first-primary')
);

export const test_d1_api_happy_path_withsessions_some_ranomd_token = test(
  testD1ApiQueriesHappyPath,
  (env) => getDBFromEnv(env).withSession('token-doesnot-matter-for-now')
);

async function testD1ApiQueriesHappyPath(DB) {
  await itShould(
    'create a Users table',
    () =>
      DB.prepare(
        ` CREATE TABLE users
        (
            user_id    INTEGER PRIMARY KEY,
            name       TEXT,
            home       TEXT,
            features   TEXT,
            land_based BOOLEAN
        );`
      ).run(),
    { success: true, meta: anything }
  );

  await itShould(
    'select an empty set',
    () => DB.prepare(`SELECT * FROM users;`).all(),
    {
      success: true,
      results: [],
      meta: meta({ changed_db: false }),
    }
  );

  await itShould(
    'have no results for .run()',
    () => DB.prepare(`SELECT * FROM users;`).run(),
    { success: true, meta: anything }
  );

  await itShould(
    'delete no rows ok',
    () => DB.prepare(`DELETE FROM users;`).run(),
    { success: true, meta: anything }
  );

  await itShould(
    'insert a few rows with a returning statement',
    () =>
      DB.prepare(
        `
        INSERT INTO users (name, home, features, land_based) VALUES
          ('Albert Ross', 'sky', 'wingspan', false),
          ('Al Dente', 'bowl', 'mouthfeel', true)
        RETURNING *
    `
      ).all(),
    {
      success: true,
      results: [
        {
          user_id: 1,
          name: 'Albert Ross',
          home: 'sky',
          features: 'wingspan',
          land_based: 0,
        },
        {
          user_id: 2,
          name: 'Al Dente',
          home: 'bowl',
          features: 'mouthfeel',
          land_based: 1,
        },
      ],
      meta: anything,
    }
  );

  await itShould(
    'delete two rows ok',
    () => DB.prepare(`DELETE FROM users;`).run(),
    { success: true, meta: anything }
  );

  // In an earlier implementation, .run() called a different endpoint that threw on RETURNING clauses.
  await itShould(
    'insert a few rows with a returning statement, but ignore the result without erroring',
    () =>
      DB.prepare(
        `
        INSERT INTO users (name, home, features, land_based) VALUES
          ('Albert Ross', 'sky', 'wingspan', false),
          ('Al Dente', 'bowl', 'mouthfeel', true)
        RETURNING *
    `
      ).run(),
    {
      success: true,
      meta: anything,
    }
  );

  // Results format tests

  const select_1 = DB.prepare(`select 1;`);
  await itShould(
    'return simple results for select 1',
    () => select_1.all(),
    {
      results: [{ 1: 1 }],
      meta: anything,
      success: true,
    },
    () => select_1.raw(),
    [[1]],
    () => select_1.first(),
    { 1: 1 },
    () => select_1.first('1'),
    1
  );

  const select_all = DB.prepare(`SELECT * FROM users;`);
  await itShould(
    'return all users',
    () => select_all.all(),
    {
      results: [
        {
          user_id: 1,
          name: 'Albert Ross',
          home: 'sky',
          features: 'wingspan',
          land_based: 0,
        },
        {
          user_id: 2,
          name: 'Al Dente',
          home: 'bowl',
          features: 'mouthfeel',
          land_based: 1,
        },
      ],
      meta: anything,
      success: true,
    },
    () => select_all.raw(),
    [
      [1, 'Albert Ross', 'sky', 'wingspan', 0],
      [2, 'Al Dente', 'bowl', 'mouthfeel', 1],
    ],
    () => select_all.first(),
    {
      user_id: 1,
      name: 'Albert Ross',
      home: 'sky',
      features: 'wingspan',
      land_based: 0,
    },
    () => select_all.first('name'),
    'Albert Ross'
  );

  const select_one = DB.prepare(`SELECT * FROM users WHERE user_id = ?;`);

  await itShould(
    'return the first user when bound with user_id = 1',
    () => select_one.bind(1).all(),
    {
      results: [
        {
          user_id: 1,
          name: 'Albert Ross',
          home: 'sky',
          features: 'wingspan',
          land_based: 0,
        },
      ],
      meta: anything,
      success: true,
    },
    () => select_one.bind(1).raw(),
    [[1, 'Albert Ross', 'sky', 'wingspan', 0]],
    () => select_one.bind(1).first(),
    {
      user_id: 1,
      name: 'Albert Ross',
      home: 'sky',
      features: 'wingspan',
      land_based: 0,
    },
    () => select_one.bind(1).first('name'),
    'Albert Ross'
  );

  await itShould(
    'return the second user when bound with user_id = 2',
    () => select_one.bind(2).all(),
    {
      results: [
        {
          user_id: 2,
          name: 'Al Dente',
          home: 'bowl',
          features: 'mouthfeel',
          land_based: 1,
        },
      ],
      meta: anything,
      success: true,
    },
    () => select_one.bind(2).raw(),
    [[2, 'Al Dente', 'bowl', 'mouthfeel', 1]],
    () => select_one.bind(2).first(),
    {
      user_id: 2,
      name: 'Al Dente',
      home: 'bowl',
      features: 'mouthfeel',
      land_based: 1,
    },
    () => select_one.bind(2).first('name'),
    'Al Dente'
  );

  await itShould(
    'return the results of two commands with batch',
    () => DB.batch([select_one.bind(2), select_one.bind(1)]),
    [
      {
        results: [
          {
            user_id: 2,
            name: 'Al Dente',
            home: 'bowl',
            features: 'mouthfeel',
            land_based: 1,
          },
        ],
        meta: anything,
        success: true,
      },
      {
        results: [
          {
            user_id: 1,
            name: 'Albert Ross',
            home: 'sky',
            features: 'wingspan',
            land_based: 0,
          },
        ],
        meta: anything,
        success: true,
      },
    ]
  );

  await itShould(
    'allow binding all types of parameters',
    () =>
      DB.prepare(`SELECT count(1) as count FROM users WHERE land_based = ?`)
        .bind(true)
        .first('count'),
    1,
    () =>
      DB.prepare(`SELECT count(1) as count FROM users WHERE land_based = ?`)
        .bind(false)
        .first('count'),
    1,
    () =>
      DB.prepare(`SELECT count(1) as count FROM users WHERE land_based = ?`)
        .bind(0)
        .first('count'),
    1,
    () =>
      DB.prepare(`SELECT count(1) as count FROM users WHERE land_based = ?`)
        .bind(1)
        .first('count'),
    1,
    () =>
      DB.prepare(`SELECT count(1) as count FROM users WHERE land_based = ?`)
        .bind(2)
        .first('count'),
    0
  );

  await itShould(
    'create two tables with overlapping column names',
    () =>
      DB.batch([
        DB.prepare(`CREATE TABLE abc (a INT, b INT, c INT);`),
        DB.prepare(`CREATE TABLE cde (c TEXT, d TEXT, e TEXT);`),
        DB.prepare(`INSERT INTO abc VALUES (1,2,3),(4,5,6);`),
        DB.prepare(
          `INSERT INTO cde VALUES ("A", "B", "C"),("D","E","F"),("G","H","I");`
        ),
      ]),
    [
      {
        success: true,
        results: [],
        meta: meta({
          changed_db: true,
          changes: 0,
          last_row_id: 2,
          rows_read: 1,
          rows_written: 2,
        }),
      },
      {
        success: true,
        results: [],
        meta: meta({
          changed_db: true,
          changes: 0,
          last_row_id: 2,
          rows_read: 1,
          rows_written: 2,
        }),
      },
      {
        success: true,
        results: [],
        meta: meta({
          changed_db: true,
          changes: 2,
          last_row_id: 2,
          rows_read: 0,
          rows_written: 2,
        }),
      },
      {
        success: true,
        results: [],
        meta: meta({
          changed_db: true,
          changes: 3,
          last_row_id: 3,
          rows_read: 0,
          rows_written: 3,
        }),
      },
    ]
  );

  await itShould(
    'still sadly lose data for duplicate columns in a join',
    () => DB.prepare(`SELECT * FROM abc, cde;`).all(),
    {
      success: true,
      results: [
        { a: 1, b: 2, c: 'A', d: 'B', e: 'C' },
        { a: 1, b: 2, c: 'D', d: 'E', e: 'F' },
        { a: 1, b: 2, c: 'G', d: 'H', e: 'I' },
        { a: 4, b: 5, c: 'A', d: 'B', e: 'C' },
        { a: 4, b: 5, c: 'D', d: 'E', e: 'F' },
        { a: 4, b: 5, c: 'G', d: 'H', e: 'I' },
      ],
      meta: meta({
        changed_db: false,
        changes: 0,
        rows_read: 8,
        rows_written: 0,
      }),
    }
  );

  await itShould(
    'not lose data for duplicate columns in a join using raw()',
    () => DB.prepare(`SELECT * FROM abc, cde;`).raw(),
    [
      [1, 2, 3, 'A', 'B', 'C'],
      [1, 2, 3, 'D', 'E', 'F'],
      [1, 2, 3, 'G', 'H', 'I'],
      [4, 5, 6, 'A', 'B', 'C'],
      [4, 5, 6, 'D', 'E', 'F'],
      [4, 5, 6, 'G', 'H', 'I'],
    ]
  );

  await itShould(
    'add columns using  .raw({ columnNames: true })',
    () => DB.prepare(`SELECT * FROM abc, cde;`).raw({ columnNames: true }),
    [
      ['a', 'b', 'c', 'c', 'd', 'e'],
      [1, 2, 3, 'A', 'B', 'C'],
      [1, 2, 3, 'D', 'E', 'F'],
      [1, 2, 3, 'G', 'H', 'I'],
      [4, 5, 6, 'A', 'B', 'C'],
      [4, 5, 6, 'D', 'E', 'F'],
      [4, 5, 6, 'G', 'H', 'I'],
    ]
  );

  await itShould(
    'not add columns using  .raw({ columnNames: false })',
    () => DB.prepare(`SELECT * FROM abc, cde;`).raw({ columnNames: false }),
    [
      [1, 2, 3, 'A', 'B', 'C'],
      [1, 2, 3, 'D', 'E', 'F'],
      [1, 2, 3, 'G', 'H', 'I'],
      [4, 5, 6, 'A', 'B', 'C'],
      [4, 5, 6, 'D', 'E', 'F'],
      [4, 5, 6, 'G', 'H', 'I'],
    ]
  );

  await itShould(
    'return 0 rows_written for IN clauses',
    () =>
      DB.prepare(
        `SELECT * from cde WHERE c IN ('A','B','C','X','Y','Z')`
      ).all(),
    {
      success: true,
      results: [{ c: 'A', d: 'B', e: 'C' }],
      meta: meta({ rows_read: 3, rows_written: 0 }),
    }
  );

  await itShould(
    'delete all created tables',
    () =>
      DB.batch([
        DB.prepare(`DROP TABLE users;`),
        DB.prepare(`DROP TABLE abc;`),
        DB.prepare(`DROP TABLE cde;`),
      ]),
    [
      {
        success: true,
        results: [],
        meta: meta({
          changed_db: true,
          changes: 0,
          last_row_id: 3,
          rows_read: 4,
          rows_written: 0,
        }),
      },
      {
        success: true,
        results: [],
        meta: meta({
          changed_db: true,
          changes: 0,
          last_row_id: 3,
          rows_read: 3,
          rows_written: 0,
        }),
      },
      {
        success: true,
        results: [],
        meta: meta({
          changed_db: true,
          changes: 0,
          last_row_id: 3,
          rows_read: 2,
          rows_written: 0,
        }),
      },
    ]
  );
}

// envD1MockFetcher is the default export worker in `d1-mock.js`, i.e. the `fetch()` entry point.
const getCommitTokensSentFromBinding = async (envD1MockFetcher) =>
  (
    await (
      await envD1MockFetcher.fetch(`http://d1-api-test/commitTokens`)
    ).json()
  ).commitTokensReceived;
const getCommitTokensReturnedFromEyeball = async (envD1MockFetcher) =>
  (
    await (
      await envD1MockFetcher.fetch(`http://d1-api-test/commitTokens`)
    ).json()
  ).commitTokensReturned;
const resetCommitTokens = async (envD1MockFetcher) =>
  await (
    await envD1MockFetcher.fetch(`http://d1-api-test/commitTokens/reset`)
  ).json();
const setNextCommitTokenFromEyeball = async (envD1MockFetcher, t) =>
  await (
    await envD1MockFetcher.fetch(
      `http://d1-api-test/commitTokens/nextToken?t=${t}`
    )
  ).json();

export const test_d1_api_withsessions_token_handling = test(
  testD1ApiWithSessionsTokensHandling,
  getDBFromEnv
);

async function testD1ApiWithSessionsTokensHandling(DB, envD1MockFetcher) {
  const assertTokensSentReceived = async (firstTokenFromBinding) => {
    const tokens = await getCommitTokensSentFromBinding(envD1MockFetcher);
    assert.deepEqual(tokens[0], firstTokenFromBinding);
    // Make sure we sent back whatever we received from the previous query.
    assert.deepEqual(
      tokens.slice(1),
      (await getCommitTokensReturnedFromEyeball(envD1MockFetcher)).slice(0, -1)
    );
  };

  // Assert tokens sent by the top level DB are always primary!
  await resetCommitTokens(envD1MockFetcher);
  await testD1ApiQueriesHappyPath(DB);
  let tokens = await getCommitTokensSentFromBinding(envD1MockFetcher);
  assert.deepEqual(
    tokens.every((t) => t === 'first-primary'),
    true
  );
  // Make sure we received different tokens, and still sent first-primary.
  assert.deepEqual(
    (await getCommitTokensReturnedFromEyeball(envD1MockFetcher)).every(
      (t) => t !== 'first-primary'
    ),
    true
  );

  // Assert tokens sent by the DEFAULT DB.withSessions()
  await resetCommitTokens(envD1MockFetcher);
  await testD1ApiQueriesHappyPath(DB.withSession());
  await assertTokensSentReceived('first-unconstrained');

  // Assert tokens sent by the DB.withSessions("first-unconstrained")
  await resetCommitTokens(envD1MockFetcher);
  await testD1ApiQueriesHappyPath(DB.withSession('first-unconstrained'));
  await assertTokensSentReceived('first-unconstrained');

  // Assert tokens sent by the DB.withSessions("first-primary")
  await resetCommitTokens(envD1MockFetcher);
  await testD1ApiQueriesHappyPath(DB.withSession('first-primary'));
  await assertTokensSentReceived('first-primary');
}

export const test_d1_api_withsessions_old_token_skipped = test(
  testD1ApiWithSessionsOldTokensSkipped,
  getDBFromEnv
);

async function testD1ApiWithSessionsOldTokensSkipped(DB, envD1MockFetcher) {
  const runTest = async (session) => {
    await resetCommitTokens(envD1MockFetcher);
    await session.prepare(`SELECT * FROM sqlite_master;`).all();
    await session.prepare(`SELECT * FROM sqlite_master;`).all();

    // This is alphanumerically smaller than the tokens generated by d1-mock.js.
    await setNextCommitTokenFromEyeball(envD1MockFetcher, '------');

    // The token from this should be ignored!
    await session.prepare(`SELECT * FROM sqlite_master;`).all();

    // But not from this since a normal larger value should be received.
    await session.prepare(`SELECT * FROM sqlite_master;`).all();
    await session.prepare(`SELECT * FROM sqlite_master;`).all();

    const tokensFromBinding =
      await getCommitTokensSentFromBinding(envD1MockFetcher);
    const tokensFromEyeball =
      await getCommitTokensReturnedFromEyeball(envD1MockFetcher);
    const expectedTokensFromBinding = [
      'first-unconstrained',
      tokensFromEyeball[0],
      tokensFromEyeball[1],
      // We skip the commit token "------", since the previously received one was more recent.
      tokensFromEyeball[1],
      // The binding then sents back the next largest value.
      tokensFromEyeball[3],
    ];
    assert.deepEqual(tokensFromBinding, expectedTokensFromBinding);

    return { ok: true };
  };

  itShould('default DB', runTest(DB), { ok: true });
  itShould('withSession()', runTest(DB.withSession()), { ok: true });
  itShould(
    'withSession(first-unconstrained)',
    runTest(DB.withSession('first-unconstrained')),
    { ok: true }
  );
  itShould(
    'withSession(first-primary)',
    runTest(DB.withSession('first-primary')),
    { ok: true }
  );
}
