from cloudflare.workers import Response


async def on_fetch(request):
    if request.url.endswith("/sub"):
        return Response("Hi there!")
    elif request.url.endswith("/sub_headers"):
        assert request.headers.get("Custom-Req-Header") == "123"
        return Response(
            "Hi there!", headers={"Custom-Header-That-Should-Passthrough": True}
        )
    else:
        raise ValueError("Unexpected path " + request.url)


def test():
    pass
