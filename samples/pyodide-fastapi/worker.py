from asgi import env

async def on_fetch(request):
    import asgi

    return await asgi.fetch(app, request, env)


def test():
    import fastapi


# Set up fastapi app

from fastapi import FastAPI
from pydantic import BaseModel


app = FastAPI()


@app.get("/hello")
async def root(env=env):
    return {"message": "Hello World", "secret": env.secret}


@app.get("/route")
async def root():
    return {"message": "this is my custom route"}


@app.get("/favicon.ico")
async def root():
    return {"message": "here's a favicon I guess?"}


@app.get("/items/{item_id}")
async def read_item(item_id: int):
    return {"item_id": item_id}


class Item(BaseModel):
    name: str
    description: str | None = None
    price: float
    tax: float | None = None


@app.post("/items/")
async def create_item(item: Item):
    return item


@app.put("/items/{item_id}")
async def create_item(item_id: int, item: Item, q: str | None = None):
    result = {"item_id": item_id, **item.dict()}
    if q:
        result.update({"q": q})
    return result
