from cloudflare.workers import FormData, Response


async def on_fetch(request):
    if request.url.endswith("/sub"):
        return Response("Hi there!")
    elif request.url.endswith("/sub_headers"):
        assert request.headers.get("Custom-Req-Header") == "123"
        return Response(
            "Hi there!", headers={"Custom-Header-That-Should-Passthrough": True}
        )
    elif request.url.endswith("/redirect"):
        return Response.redirect("https://example.com/sub", status=301)
    elif request.url.endswith("/formdata"):
        data = FormData({"field": "value"})
        return Response(data)
    else:
        raise ValueError("Unexpected path " + request.url)


def test():
    pass
