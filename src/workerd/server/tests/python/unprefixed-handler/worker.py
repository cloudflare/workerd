from workers import Response, handler


@handler
def fetch():
    return Response("hello world")


async def test(ctrl, env, ctx):
    response = await env.SELF.fetch(
        "http://example.com/",
    )
    text = await response.text()
    assert text == "hello world"
