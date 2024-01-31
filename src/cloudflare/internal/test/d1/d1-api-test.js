// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert'

// Test helpers, since I want everything to run in sequence but I don't
// want to lose context about which assertion failed.
const test = (fn) => ({
  async test(ctr, env) {
    await fn(env.d1)
  },
})

// Recurse through nested objects/arrays looking for 'anything' and deleting that
// key/value from both objects. Gives us a way to get expect.toMatchObject behaviour
// with only deepEqual
const anything = Symbol('anything')
const deleteAnything = (expected, actual) => {
  Object.entries(expected).forEach(([k, v]) => {
    if (v === anything) {
      delete actual[k]
      delete expected[k]
    } else if (typeof v === 'object' && typeof actual[k] === 'object') {
      deleteAnything(expected[k], actual[k])
    }
  })
}

const itShould = async (description, ...assertions) => {
  if (assertions.length % 2 !== 0)
    throw new Error('itShould takes pairs of cb, expected args')

  try {
    for (let i = 0; i < assertions.length; i += 2) {
      const cb = assertions[i]
      const expected = assertions[i + 1]
      const actual = await cb()
      deleteAnything(expected, actual)
      try {
        assert.deepEqual(actual, expected)
      } catch (e) {
        console.log(actual)
        throw e
      }
    }
  } catch (e) {
    throw new Error(`TEST ERROR!\n❌ Failed to ${description}\n${e.message}`)
  }
}

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
})

export const test_d1_api = test(async (DB) => {
  await itShould(
    'create a Users table',
    () =>
      DB.prepare(
        ` CREATE TABLE users
        (
            user_id  INTEGER PRIMARY KEY,
            name     TEXT,
            home     TEXT,
            features TEXT
        );`
      ).run(),
    { success: true, meta: anything }
  )

  await itShould(
    'select an empty set',
    () => DB.prepare(`SELECT * FROM users;`).all(),
    {
      success: true,
      results: [],
      meta: meta({ changed_db: false }),
    }
  )

  await itShould(
    'have no results for .run()',
    () => DB.prepare(`SELECT * FROM users;`).run(),
    { success: true, meta: anything }
  )

  await itShould(
    'delete no rows ok',
    () => DB.prepare(`DELETE FROM users;`).run(),
    { success: true, meta: anything }
  )

  await itShould(
    'insert a few rows with a returning statement',
    () =>
      DB.prepare(
        `
        INSERT INTO users (name, home, features) VALUES
          ('Albert Ross', 'sky', 'wingspan'),
          ('Al Dente', 'bowl', 'mouthfeel')
        RETURNING *
    `
      ).all(),
    {
      success: true,
      results: [
        { user_id: 1, name: 'Albert Ross', home: 'sky', features: 'wingspan' },
        { user_id: 2, name: 'Al Dente', home: 'bowl', features: 'mouthfeel' },
      ],
      meta: anything,
    }
  )

  await itShould(
    'delete two rows ok',
    () => DB.prepare(`DELETE FROM users;`).run(),
    { success: true, meta: anything }
  )

  // In an earlier implementation, .run() called a different endpoint that threw on RETURNING clauses.
  await itShould(
    'insert a few rows with a returning statement, but ignore the result without erroring',
    () =>
      DB.prepare(
        `
        INSERT INTO users (name, home, features) VALUES
          ('Albert Ross', 'sky', 'wingspan'),
          ('Al Dente', 'bowl', 'mouthfeel')
        RETURNING *
    `
      ).run(),
    {
      success: true,
      meta: anything,
    }
  )

  // Results format tests

  const select_1 = DB.prepare(`select 1;`)
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
  )

  const select_all = DB.prepare(`SELECT * FROM users;`)
  await itShould(
    'return all users',
    () => select_all.all(),
    {
      results: [
        { user_id: 1, name: 'Albert Ross', home: 'sky', features: 'wingspan' },
        { user_id: 2, name: 'Al Dente', home: 'bowl', features: 'mouthfeel' },
      ],
      meta: anything,
      success: true,
    },
    () => select_all.raw(),
    [
      [1, 'Albert Ross', 'sky', 'wingspan'],
      [2, 'Al Dente', 'bowl', 'mouthfeel'],
    ],
    () => select_all.first(),
    { user_id: 1, name: 'Albert Ross', home: 'sky', features: 'wingspan' },
    () => select_all.first('name'),
    'Albert Ross'
  )

  const select_one = DB.prepare(`SELECT * FROM users WHERE user_id = ?;`)

  await itShould(
    'return the first user when bound with user_id = 1',
    () => select_one.bind(1).all(),
    {
      results: [
        { user_id: 1, name: 'Albert Ross', home: 'sky', features: 'wingspan' },
      ],
      meta: anything,
      success: true,
    },
    () => select_one.bind(1).raw(),
    [[1, 'Albert Ross', 'sky', 'wingspan']],
    () => select_one.bind(1).first(),
    { user_id: 1, name: 'Albert Ross', home: 'sky', features: 'wingspan' },
    () => select_one.bind(1).first('name'),
    'Albert Ross'
  )

  await itShould(
    'return the second user when bound with user_id = 2',
    () => select_one.bind(2).all(),
    {
      results: [
        { user_id: 2, name: 'Al Dente', home: 'bowl', features: 'mouthfeel' },
      ],
      meta: anything,
      success: true,
    },
    () => select_one.bind(2).raw(),
    [[2, 'Al Dente', 'bowl', 'mouthfeel']],
    () => select_one.bind(2).first(),
    { user_id: 2, name: 'Al Dente', home: 'bowl', features: 'mouthfeel' },
    () => select_one.bind(2).first('name'),
    'Al Dente'
  )

  await itShould(
    'return the results of two commands with batch',
    () => DB.batch([select_one.bind(2), select_one.bind(1)]),
    [
      {
        results: [
          { user_id: 2, name: 'Al Dente', home: 'bowl', features: 'mouthfeel' },
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
          },
        ],
        meta: anything,
        success: true,
      },
    ]
  )

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
  )

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
  )

  await itShould(
    'not lose data for duplicate columns in a join using raw',
    () => DB.prepare(`SELECT * FROM abc, cde;`).raw(),
    [
      [1, 2, 3, 'A', 'B', 'C'],
      [1, 2, 3, 'D', 'E', 'F'],
      [1, 2, 3, 'G', 'H', 'I'],
      [4, 5, 6, 'A', 'B', 'C'],
      [4, 5, 6, 'D', 'E', 'F'],
      [4, 5, 6, 'G', 'H', 'I'],
    ]
  )

  await itShould(
    'return 0 rows_written for IN clauses',
    () => DB.prepare(`SELECT * from cde WHERE c IN ('A','B','C','X','Y','Z')`).all(),
    {
      success: true,
      results: [{ c: 'A', d: 'B', e: 'C' }],
      meta: meta({ rows_read: 3, rows_written: 0 }),
    }
  )
})
