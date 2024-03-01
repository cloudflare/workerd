async def on_fetch(request):
    import asgi

    return await asgi.fetch(app, request)


def test():
    import fastapi


# Set up fastapi app

from fastapi import FastAPI
from pydantic import BaseModel


my_awesome_api = FastAPI()
app = my_awesome_api


@my_awesome_api.get("/hello")
async def root():
    return {"message": "Hello World"}


@my_awesome_api.get("/route")
async def root():
    return {"message": "this is my custom route"}


@my_awesome_api.get("/favicon.ico")
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
