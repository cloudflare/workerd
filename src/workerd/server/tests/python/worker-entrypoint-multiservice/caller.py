async def test(ctrl, env, ctx):
    result = await env.RPC.test_function("Hello from Python")
    assert result == "Received: Hello from Python", f"Unexpected RPC result: {result}"

    first_resp = await env.entrypoint.rpc_trigger_func()
    assert first_resp == "hello from python 1"

    second_resp = await env.entrypoint.rpc_trigger_func()
    # We expect the counter to stay the same since this isn't a durable object.
    assert second_resp == "hello from python 1"
