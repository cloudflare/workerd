from js import Response


def fetch(request):
    return Response.new("hello world")


def test():
    print("Hi there, this is a test")
