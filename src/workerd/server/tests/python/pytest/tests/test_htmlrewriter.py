import asyncio

import pytest
from workers import HTMLRewriter, Response

from pyodide.ffi import JsException


@pytest.mark.asyncio
async def test_sync_element_handler():
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


@pytest.mark.asyncio
async def test_async_element_handler():
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


@pytest.mark.asyncio
async def test_document_handler():
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


@pytest.mark.asyncio
async def test_multiple_handlers():
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
    text = await result.text()

    assert 'data-h1="true"' in text, f"Handler1 didn't work: {text}"
    assert 'data-h2="true"' in text, f"Handler2 didn't work: {text}"


@pytest.mark.asyncio
async def test_rewriter_reuse():
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

    result1 = rewriter.transform(Response(html, headers={"content-type": "text/html"}))
    text1 = await result1.text()
    assert 'data-count="1"' in text1, f"First transform failed: {text1}"
    assert counter.count == 1

    result2 = rewriter.transform(Response(html, headers={"content-type": "text/html"}))
    text2 = await result2.text()
    assert 'data-count="2"' in text2, f"Second transform failed: {text2}"
    assert counter.count == 2


@pytest.mark.asyncio
async def test_stream_cancellation():
    html = "<html><body><div>Test</div></body></html>"
    response = Response(html, headers={"content-type": "text/html"})

    class Handler:
        def __init__(self):
            self.called = False

        def element(self, el):
            self.called = True

    handler = Handler()
    rewriter = HTMLRewriter()
    rewriter.on("div", handler)

    result = rewriter.transform(response)

    body = result.js_object.body
    reader = body.getReader()
    await reader.cancel()

    try:
        await reader.read()
    except Exception:
        pass  # Expected - stream was cancelled


def is_proxy_destroyed(proxy) -> bool:
    try:
        return not getattr(proxy, "_active", True)
    except JsException as e:
        return "Object has already been destroyed" in str(e)


@pytest.mark.asyncio
async def test_proxy_cleanup_on_completion():
    html = "<html><body><div>Test</div></body></html>"
    response = Response(html, headers={"content-type": "text/html"})

    class Handler:
        def element(self, el):
            el.setAttribute("data-test", "true")

    rewriter = HTMLRewriter()
    rewriter.on("div", Handler())
    result = rewriter.transform(response)

    proxies = rewriter._last_handler_proxies
    assert proxies is not None
    assert len(proxies) == 3  # 1 handler + 2 internal proxies

    for proxy in proxies:
        assert not is_proxy_destroyed(proxy), "Proxy should be alive before consumption"

    text = await result.text()
    assert 'data-test="true"' in text

    for proxy in proxies:
        assert is_proxy_destroyed(proxy), "Proxy should be destroyed after consumption"


@pytest.mark.asyncio
async def test_proxy_cleanup_on_cancel():
    html = "<html><body><div>Test</div></body></html>"
    response = Response(html, headers={"content-type": "text/html"})

    class Handler:
        def element(self, el):
            el.setAttribute("data-test", "true")

    rewriter = HTMLRewriter()
    rewriter.on("div", Handler())
    result = rewriter.transform(response)

    proxies = rewriter._last_handler_proxies
    assert proxies is not None
    assert len(proxies) == 3  # 1 handler + 2 internal proxies

    for proxy in proxies:
        assert not is_proxy_destroyed(proxy), "Proxy should be alive before cancel"

    body = result.js_object.body
    reader = body.getReader()
    await reader.cancel()

    try:
        await reader.read()
    except Exception:
        pass  # Expected - stream was cancelled

    for proxy in proxies:
        assert is_proxy_destroyed(proxy), "Proxy should be destroyed after cancel"


@pytest.mark.asyncio
async def test_exception_handling():
    html = "<html><body><div>Test</div></body></html>"
    response = Response(html, headers={"content-type": "text/html"})

    class Handler:
        def element(self, el):
            el.setAttribute("data-test", "true")
            raise RuntimeError("Test exception")

    rewriter = HTMLRewriter()
    rewriter.on("div", Handler())
    result = rewriter.transform(response)

    try:
        await result.text()
    except Exception as e:
        assert "PythonError" in str(e)
