from js import Response
from workers import WorkerEntrypoint


class Default(WorkerEntrypoint):
    def fetch(self, request):
        return Response.new("hello world")


def test():
    print("Hi there, this is a test")
