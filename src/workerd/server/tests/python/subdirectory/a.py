from js import Response
from subdir.a import x
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    def fetch(self, request):
        return Response.new("hello world")

    def test(self):
        print("Hi there, this is a test", x)
