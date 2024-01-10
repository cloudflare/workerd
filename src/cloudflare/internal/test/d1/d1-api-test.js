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
      assert.deepEqual(actual, expected)
    }
  } catch (e) {
    throw new Error(`TEST ERROR!\n❌ Failed to ${description}\n${e.message}`)
  }
}

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
      meta: anything,
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
      DB.exec(`
        CREATE TABLE abc (a INT, b INT, c INT);
        CREATE TABLE cde (c INT, d INT, e INT);
        INSERT INTO abc VALUES (1,2,3),(4,5,6);
        INSERT INTO cde VALUES (7,8,9),(1,2,3);
      `),
    {
      count: 4,
      duration: anything,
    }
  )

  await itShould(
    'still sadly lose data for duplicate columns in a join',
    () => DB.prepare(`SELECT * FROM abc, cde;`).all(),
    {
      success: true,
      results: [
        { a: 1, b: 2, c: 7, d: 8, e: 9 },
        { a: 1, b: 2, c: 1, d: 2, e: 3 },
        { a: 4, b: 5, c: 7, d: 8, e: 9 },
        { a: 4, b: 5, c: 1, d: 2, e: 3 },
      ],
      meta: anything,
    }
  )

  await itShould(
    'not lose data for duplicate columns in a join using raw',
    () => DB.prepare(`SELECT * FROM abc, cde;`).raw(),
    [
      [1, 2, 3, 7, 8, 9],
      [1, 2, 3, 1, 2, 3],
      [4, 5, 6, 7, 8, 9],
      [4, 5, 6, 1, 2, 3],
    ]
  )
})
