import * as assert from 'node:assert'

export const test_d1 = {
  async test(ctr, env) {
    const DB = env.d1
    {
      const stmt = DB.prepare(`select 1`)
      assert.deepEqual(await stmt.all(), {
        results: [{ 1: 1 }],
        meta: { duration: 0.001, served_by: 'd1-mock' },
        success: true,
      })

      assert.deepEqual(await stmt.raw(), [[1]])
      assert.deepEqual(await stmt.first(), { 1: 1 })
      assert.deepEqual(await stmt.first('1'), 1)
    }
  },
}
