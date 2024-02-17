from js import Response
from asyncio import sleep, ensure_future


def fetch(request):
    return Response.new("hello world")


def test(*args):
    print("going to sleep")
    ensure_future(sleep(1)).syncify()
    print("Hi there, this is a test")
