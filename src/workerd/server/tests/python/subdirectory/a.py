from js import Response
from subdir.a import x


def fetch(request):
    return Response.new("hello world")


def test():
    print("Hi there, this is a test", x)
    raise Exception("stop")
