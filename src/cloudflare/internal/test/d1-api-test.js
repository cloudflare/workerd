// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert'

// TODO: how to import this from d1-mock.js?
const MOCK_USER_ROWS = {
  1: { user_id: 1, name: 'Albert Ross', home: 'sky', features: 'wingspan' },
  2: { user_id: 2, name: 'Al Dente', home: 'bowl', features: 'mouthfeel' },
}

export const test_d1_select_1 = {
  async test(ctr, env) {
    const DB = env.d1

    const stmt = DB.prepare(`select 1;`)
    assert.deepEqual(await stmt.all(), {
      results: [{ 1: 1 }],
      meta: { duration: 0.001, served_by: 'd1-mock' },
      success: true,
    })

    const [raw, first, firstColumn] = await Promise.all([
      stmt.raw(),
      stmt.first(),
      stmt.first('1'),
    ])
    assert.deepEqual(raw, [[1]])
    assert.deepEqual(first, { 1: 1 })
    assert.deepEqual(firstColumn, 1)
  },
}

export const test_d1_select_all = {
  async test(ctr, env) {
    const DB = env.d1
    const user_1 = MOCK_USER_ROWS[1]
    const user_2 = MOCK_USER_ROWS[2]

    const stmt = DB.prepare(`select * from users;`)
    assert.deepEqual(await stmt.all(), {
      results: [user_1, user_2],
      meta: { duration: 0.001, served_by: 'd1-mock' },
      success: true,
    })

    const [raw, first, firstColumn] = await Promise.all([
      stmt.raw(),
      stmt.first(),
      stmt.first('features'),
    ])
    assert.deepEqual(raw, [Object.values(user_1), Object.values(user_2)])
    assert.deepEqual(first, user_1)
    assert.deepEqual(firstColumn, user_1.features)
  },
}

export const test_d1_select_one = {
  async test(ctr, env) {
    const DB = env.d1
    const user_1 = MOCK_USER_ROWS[1]
    const user_2 = MOCK_USER_ROWS[2]

    const withParam = DB.prepare(`select * from users where user_id = ?;`)
    {
      const stmt = withParam.bind(1)
      assert.deepEqual(await stmt.all(), {
        results: [user_1],
        meta: { duration: 0.001, served_by: 'd1-mock' },
        success: true,
      })

      const [raw, first, firstColumn] = await Promise.all([
        stmt.raw(),
        stmt.first(),
        stmt.first('home'),
      ])
      assert.deepEqual(raw, [Object.values(user_1)])
      assert.deepEqual(first, user_1)
      assert.deepEqual(firstColumn, user_1.home)
    }
    {
      const stmt = withParam.bind(2)
      assert.deepEqual(await stmt.all(), {
        results: [user_2],
        meta: { duration: 0.001, served_by: 'd1-mock' },
        success: true,
      })

      const [raw, first, firstColumn] = await Promise.all([
        stmt.raw(),
        stmt.first(),
        stmt.first('name'),
      ])
      assert.deepEqual(raw, [Object.values(user_2)])
      assert.deepEqual(first, user_2)
      assert.deepEqual(firstColumn, user_2.name)
    }
  },
}

export const test_d1_batch = {
  async test(ctr, env) {
    const DB = env.d1
    const user_1 = MOCK_USER_ROWS[1]
    const user_2 = MOCK_USER_ROWS[2]

    const withParam = DB.prepare(`select * from users where user_id = ?;`)
    const response = await DB.batch([withParam.bind(1), withParam.bind(2)])
    assert.deepEqual(response, [
      {
        results: [user_1],
        meta: { duration: 0.001, served_by: 'd1-mock' },
        success: true,
      },
      {
        results: [user_2],
        meta: { duration: 0.001, served_by: 'd1-mock' },
        success: true,
      },
    ])
  },
}

export const test_d1_exec = {
  async test(ctr, env) {
    const DB = env.d1
    const response = await DB.exec(`
      select 1;
      select * from users;
    `)
    assert.deepEqual(response, { count: 2, duration: 0.002 })
  },
}
