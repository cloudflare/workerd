import json

from js import Response
from workers import WorkerEntrypoint

DEFAULT_ITERATIONS = 500


def json_loop(n: int) -> int:
    count = 0
    for i in range(n):
        obj = {
            "id": i,
            "name": "item-" + str(i),
            "values": [i, i * 2, i * 3],
            "nested": {"flag": (i % 2 == 0), "index": i},
        }
        s = json.dumps(obj)
        parsed = json.loads(s)
        count += parsed["id"]
    return count


class Default(WorkerEntrypoint):
    async def fetch(self, request):
        iters = DEFAULT_ITERATIONS

        try:
            body_text = await request.text()
            if body_text:
                data = json.loads(body_text)
                iters = int(data.get("iters", iters))
        except Exception:
            iters = DEFAULT_ITERATIONS

        result = json_loop(iters)

        return Response.new(
            json.dumps({"result": result, "iters": iters}),
            {"content-type": "application/json"},
        )


def test():
    iters = DEFAULT_ITERATIONS
    result = json_loop(iters)
    print(f"Ran json_loop({iters}) -> {result}")
