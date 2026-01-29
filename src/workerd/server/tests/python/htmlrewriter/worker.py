import asyncio

from workers import HTMLRewriter, Response, WorkerEntrypoint

from pyodide.ffi import JsException


class Default(WorkerEntrypoint):
    async def test(self):
        """Run all tests."""
        await test_sync_element_handler()
        await test_async_element_handler()
        await test_document_handler()
        await test_proxy_cleanup_on_completion()
        await test_proxy_cleanup_multiple_handlers()
        await test_proxy_cleanup_on_cancel()
        await test_rewriter_reuse()

        # FIXME: The exception is caught correctly in Python-level,
        #        but workerd shows "uncaught exception" error which makes the test fail.
        # await test_handler_error()


async def test_sync_element_handler():
    """Test sync element handler with on()."""
    html = "<html><body><div>Hello</div></body></html>"
    response = Response(html, headers={"content-type": "text/html"})

    class Handler:
        def __init__(self):
            self.called = False

        def element(self, el):
            self.called = True
            el.setAttribute("data-modified", "true")

    handler = Handler()
    rewriter = HTMLRewriter()
    rewriter.on("div", handler)
    result = rewriter.transform(response)

    text = await result.text()
    assert handler.called, "Handler should have been called"
    assert 'data-modified="true"' in text, f"Expected modified attribute in: {text}"


async def test_async_element_handler():
    """Test async element handler."""
    html = "<html><body><div>Hello</div></body></html>"
    response = Response(html, headers={"content-type": "text/html"})

    class AsyncHandler:
        def __init__(self):
            self.called = False

        async def element(self, el):
            await asyncio.sleep(0.01)
            self.called = True
            el.setAttribute("data-async", "true")

    handler = AsyncHandler()
    rewriter = HTMLRewriter()
    rewriter.on("div", handler)
    result = rewriter.transform(response)

    text = await result.text()
    assert handler.called, "Async handler should have been called"
    assert 'data-async="true"' in text, f"Expected async attribute in: {text}"


async def test_document_handler():
    """Test document handler with onDocument()."""
    html = "<!DOCTYPE html><html><body>Test</body></html>"
    response = Response(html, headers={"content-type": "text/html"})

    class DocHandler:
        def __init__(self):
            self.saw_doctype = False
            self.saw_end = False

        def doctype(self, doctype):
            self.saw_doctype = True

        def end(self, end):
            self.saw_end = True
            end.append("<!-- end -->", html=True)

    handler = DocHandler()
    rewriter = HTMLRewriter()
    rewriter.onDocument(handler)
    result = rewriter.transform(response)

    text = await result.text()
    assert handler.saw_doctype, "Should have seen doctype"
    assert handler.saw_end, "Should have seen end"
    assert "<!-- end -->" in text, f"Expected appended content in: {text}"


def _check_proxy_destroyed(proxy):
    """Check if a proxy has been destroyed by attempting to use it."""
    try:
        # Attempt to use the proxy - destroyed proxies raise an error
        str(proxy)
    except JsException as e:
        return "Object has already been destroyed" in str(e)

    return False


async def test_proxy_cleanup_on_completion():
    """Test that proxies are cleaned up after stream is fully consumed.

    Uses both direct proxy checking and a marker object approach to verify cleanup.
    """
    html = "<html><body><div>Test</div></body></html>"
    response = Response(html, headers={"content-type": "text/html"})

    class Handler:
        def element(self, el):
            el.setAttribute("data-test", "true")

    handler = Handler()

    rewriter = HTMLRewriter()
    rewriter.on("div", handler)

    result = rewriter.transform(response)

    # After transform, proxies are created and tracked
    proxies = rewriter._last_transform_proxies
    assert proxies is not None, "Proxies should be tracked"
    assert len(proxies) == 3, f"Expected 3 handler proxies, got {len(proxies)}"

    for proxy in proxies:
        assert not _check_proxy_destroyed(proxy), (
            "Proxy should be alive before consumption"
        )

    # Fully consume the stream - this should trigger cleanup
    text = await result.text()

    # Verify handler worked
    assert 'data-test="true"' in text, f"Handler didn't work: {text}"

    # After consumption, proxy should be destroyed
    for proxy in proxies:
        assert _check_proxy_destroyed(proxy), (
            "Proxy should be destroyed after consumption"
        )


async def test_proxy_cleanup_multiple_handlers():
    """Test that all handler proxies are cleaned up."""
    html = "<html><body><div>D</div><span>S</span></body></html>"
    response = Response(html, headers={"content-type": "text/html"})

    class Handler1:
        def element(self, el):
            el.setAttribute("data-h1", "true")

    class Handler2:
        def element(self, el):
            el.setAttribute("data-h2", "true")

    rewriter = HTMLRewriter()
    rewriter.on("div", Handler1())
    rewriter.on("span", Handler2())

    result = rewriter.transform(response)

    # After transform, proxies are created
    proxies = rewriter._last_transform_proxies
    assert len(proxies) == 4, f"Expected 4 handler proxies, got {len(proxies)}"

    # All proxies should be alive before consumption
    for proxy in proxies:
        assert not _check_proxy_destroyed(proxy), (
            "Proxy should be alive before consumption"
        )

    text = await result.text()

    # Verify handlers worked
    assert 'data-h1="true"' in text, f"Handler1 didn't work: {text}"
    assert 'data-h2="true"' in text, f"Handler2 didn't work: {text}"

    # After consumption, all proxies should be destroyed
    for proxy in proxies:
        assert _check_proxy_destroyed(proxy), (
            "Proxy should be destroyed after consumption"
        )


async def test_proxy_cleanup_on_cancel():
    import pyodide_js

    pyodide_js.setDebug(True)
    """Test that proxies are cleaned up when stream is cancelled."""
    html = "<html><body><div>Test</div></body></html>"
    response = Response(html, headers={"content-type": "text/html"})

    class Handler:
        def element(self, el):
            el.setAttribute("data-test", "true")

    handler = Handler()

    rewriter = HTMLRewriter()
    rewriter.on("div", handler)

    result = rewriter.transform(response)

    # Get proxies for verification
    proxies = rewriter._last_transform_proxies
    assert len(proxies) == 3, f"Expected 3 handler proxies, got {len(proxies)}"

    # Proxy should be alive before cancel
    for proxy in proxies:
        assert not _check_proxy_destroyed(proxy), "Proxy should be alive before cancel"

    # Get the body and cancel it without fully consuming
    body = result.js_object.body
    reader = body.getReader()

    # Cancel before reading
    await reader.cancel("test cancellation")

    # Verify proxy was destroyed after cancel
    for proxy in proxies:
        assert _check_proxy_destroyed(proxy), "Proxy should be destroyed after cancel"


async def test_rewriter_reuse():
    """Test that HTMLRewriter can be reused for multiple transforms."""
    html = "<div>Test</div>"

    class Counter:
        def __init__(self):
            self.count = 0

        def element(self, el):
            self.count += 1
            el.setAttribute("data-count", str(self.count))

    counter = Counter()
    rewriter = HTMLRewriter()
    rewriter.on("div", counter)

    # First transform
    response = Response(html, headers={"content-type": "text/html"})
    result1 = rewriter.transform(response)
    text1 = await result1.text()
    assert 'data-count="1"' in text1, f"First transform failed: {text1}"
    assert counter.count == 1, "Counter should be 1 after first transform"

    # Second transform - same handler, fresh proxies
    result2 = rewriter.transform(Response(html, headers={"content-type": "text/html"}))
    text2 = await result2.text()

    assert 'data-count="2"' in text2, f"Second transform failed: {text2}"
    assert counter.count == 2, "Counter should be 2 after second transform"


async def test_handler_error():
    """Test error handling when handler raises"""
    html = "<html><body><div>Hello</div></body></html>"
    response = Response(html, headers={"content-type": "text/html"})

    class Handler:
        def __init__(self):
            self.called = False

        def text(self, el):
            self.called = True
            raise ValueError("Test error")

    try:
        handler = Handler()
        rewriter = HTMLRewriter()
        rewriter.on("div", handler)
        result = rewriter.transform(response)
        await result.text()
    except Exception as e:
        print("Exception catched:", e)
        assert "Test error" in str(e)
