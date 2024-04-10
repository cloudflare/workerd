# Copyright (c) 2023 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

def assertSuccess(ret):
  # D1 operations return an object with a `success` field.
  assert ret.success

async def test(context, env):
  DB = env.d1

  assertSuccess(
    await DB.prepare(
      """CREATE TABLE users
      (
          user_id    INTEGER PRIMARY KEY,
          name       TEXT,
          home       TEXT,
          features   TEXT,
          land_based BOOLEAN
      );
      """
    ).run()
  )

  result = await DB.prepare(
      """
        INSERT INTO users (name, home, features, land_based) VALUES
          ('Albert Ross', 'sky', 'wingspan', false),
          ('Al Dente', 'bowl', 'mouthfeel', true)
        RETURNING *
      """
    ).all()

  assertSuccess(result)
  assert len(result.results) == 2
  assert result.results[0].name == "Albert Ross"
