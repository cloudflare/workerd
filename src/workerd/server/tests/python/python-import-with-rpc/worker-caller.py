from workers import import_from_javascript


async def test():
    from js import console

    console.log("Running Python import with RPC tests...")

    # Import JavaScript module
    cloudflare_workers = await import_from_javascript("cloudflare:workers")
    env = cloudflare_workers.env

    # Test RPC functionality
    result = await env.RPC.test_function("Hello from Python")
    console.log(f"RPC test_function result: {result}")
    assert result == "Received: Hello from Python", f"Unexpected RPC result: {result}"

    # Test that the RPC target can also use imports
    import_result = await env.RPC.test_import()
    console.log(f"RPC test_import result: {import_result}")
    assert import_result == "Import worked in RPC context", (
        f"Unexpected import result: {import_result}"
    )

    console.log("All Python import with RPC tests passed!")
