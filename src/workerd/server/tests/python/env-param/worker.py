def test(ctx, env):
    assert hasattr(env, "secret")
    assert env.secret == "thisisasecret"
