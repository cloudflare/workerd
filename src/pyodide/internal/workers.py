# This module defines a Workers API for Python. It is similar to the API provided by
# JS Workers, but with changes and additions to be more idiomatic to the Python
# programming language.
import functools
import http.client
import json
from collections.abc import Generator, Iterable, MutableMapping
from contextlib import ExitStack, contextmanager
from enum import StrEnum
from http import HTTPMethod, HTTPStatus
from typing import Any, TypedDict, Unpack

import js

import pyodide.http
from pyodide.ffi import JsException, create_proxy, destroy_proxies, to_js
from pyodide.http import pyfetch

JSBody = (
    "js.Blob | js.ArrayBuffer | js.TypedArray | js.DataView | js.FormData |"
    "js.ReadableStream | js.URLSearchParams"
)
Body = "str | FormData | JSBody"
Headers = "dict[str, str] | list[tuple[str, str]] | js.Headers"


# https://developers.cloudflare.com/workers/runtime-apis/request/#the-cf-property-requestinitcfproperties
class RequestInitCfProperties(TypedDict, total=False):
    apps: bool | None
    cacheEverything: bool | None
    cacheKey: str | None
    cacheTags: list[str] | None
    cacheTtl: int
    cacheTtlByStatus: dict[str, int]
    image: (
        Any | None
    )  # TODO: https://developers.cloudflare.com/images/transform-images/transform-via-workers/
    mirage: bool | None
    polish: str | None
    resolveOverride: str | None
    scrapeShield: bool | None
    webp: bool | None


# This matches the Request options:
# https://developers.cloudflare.com/workers/runtime-apis/request/#options
class FetchKwargs(TypedDict, total=False):
    headers: "Headers | None"
    body: "Body | None"
    method: HTTPMethod = HTTPMethod.GET
    redirect: str | None
    cf: RequestInitCfProperties | None


# TODO: Pyodide's FetchResponse.headers returns a dict[str, str] which means
#       duplicates are lost, we should fix that so it returns a http.client.HTTPMessage
class FetchResponse(pyodide.http.FetchResponse):
    # TODO: Consider upstreaming the `body` attribute
    # TODO: Behind a compat flag make this return a native stream (StreamReader?), or perhaps
    #       behind a different name, maybe `stream`?
    @property
    def body(self) -> "js.ReadableStream":
        """
        Returns the body as a JavaScript ReadableStream from the JavaScript Response instance.
        """
        return self.js_response.body

    @property
    def js_object(self) -> "js.Response":
        return self.js_response

    """
    Instance methods defined below.

    Some methods are implemented by `FetchResponse`, these include `buffer`
    (replacing JavaScript's `arrayBuffer`), `bytes`, `json`, and `text`.

    There are also some additional methods implemented by `FetchResponse`.
    See https://pyodide.org/en/stable/usage/api/python-api/http.html#pyodide.http.FetchResponse
    for details.
    """

    async def formData(self) -> "FormData":  # TODO: Remove after certain compat date.
        return await self.form_data()

    async def form_data(self) -> "FormData":
        self._raise_if_failed()
        try:
            return FormData(await self.js_response.formData())
        except JsException as exc:
            raise _to_python_exception(exc) from exc

    def replace_body(self, body: Body) -> "FetchResponse":
        """
        Returns a new Response object with the same options (status, headers, etc) as
        the original but with an updated body.
        """
        b = body.js_object if isinstance(body, FormData) else body
        js_resp = js.Response.new(b, self.js_response)
        return FetchResponse(js_resp.url, js_resp)

    async def blob(self) -> "Blob":
        self._raise_if_failed()
        return Blob(await self.js_object.blob())


async def fetch(
    resource: str,
    **other_options: Unpack[FetchKwargs],
) -> FetchResponse:
    if "method" in other_options and isinstance(other_options["method"], HTTPMethod):
        other_options["method"] = other_options["method"].value
    resp = await pyfetch(resource, **other_options)
    return FetchResponse(resp.url, resp.js_response)


def _to_python_exception(exc: JsException) -> Exception:
    if exc.name == "RangeError":
        return ValueError(exc.message)
    elif exc.name == "TypeError":
        return TypeError(exc.message)
    else:
        return exc


@contextmanager
def _manage_pyproxies():
    proxies = js.Array.new()
    try:
        yield proxies
    finally:
        destroy_proxies(proxies)


def _to_js_headers(headers: Headers):
    if isinstance(headers, list):
        # We should have a list[tuple[str, str]]
        return js.Headers.new(headers)
    elif isinstance(headers, dict):
        return js.Headers.new(headers.items())
    elif hasattr(headers, "constructor") and headers.constructor.name == "Headers":
        return headers
    else:
        raise TypeError("Received unexpected type for headers argument")


class Response(FetchResponse):
    def __init__(
        self,
        body: Body,
        status: HTTPStatus | int = HTTPStatus.OK,
        status_text="",
        headers: Headers = None,
    ):
        """
        Represents the response to a request.

        Based on the JS API of the same name:
        https://developer.mozilla.org/en-US/docs/Web/API/Response/Response.
        """
        options = self._create_options(status, status_text, headers)

        # Initialize via the FetchResponse super-class which gives us access to
        # methods that we would ordinarily have to redeclare.
        js_resp = js.Response.new(
            body.js_object if isinstance(body, FormData) else body, **options
        )
        super().__init__(js_resp.url, js_resp)

    @staticmethod
    def _create_options(
        status: HTTPStatus | int = HTTPStatus.OK,
        status_text="",
        headers: Headers = None,
    ):
        options = {
            "status": status.value if isinstance(status, HTTPStatus) else status,
        }
        if status_text:
            options["statusText"] = status_text
        if headers:
            options["headers"] = _to_js_headers(headers)

        return options

    """
    Static methods defined below. The `error` static method is not implemented as
    it is not useful for the Workers use case.
    """

    @staticmethod
    def redirect(url: str, status: HTTPStatus | int = HTTPStatus.FOUND):
        code = status.value if isinstance(status, HTTPStatus) else status
        try:
            return js.Response.redirect(url, code)
        except JsException as exc:
            raise _to_python_exception(exc) from exc

    @staticmethod
    def json(
        data: str | dict[str, str],
        status: HTTPStatus | int = HTTPStatus.OK,
        status_text="",
        headers: Headers = None,
    ):
        options = Response._create_options(status, status_text, headers)
        with _manage_pyproxies() as pyproxies:
            try:
                return js.Response.json(
                    to_js(
                        data, dict_converter=js.Object.fromEntries, pyproxies=pyproxies
                    ),
                    **options,
                )
            except JsException as exc:
                raise _to_python_exception(exc) from exc


FormDataValue = "str | js.Blob | Blob"


def _py_value_to_js(item: FormDataValue) -> "str | js.Blob":
    if isinstance(item, Blob):
        return item.js_object
    else:
        return item


def _js_value_to_py(item: FormDataValue) -> "str | Blob | File":
    if hasattr(item, "constructor") and (item.constructor.name in ("Blob", "File")):
        if item.constructor.name == "File":
            return File(item, item.name)
        else:
            return Blob(item)
    else:
        return item


class FormData(MutableMapping[str, FormDataValue]):
    """
    This API follows that of https://pypi.org/project/multidict/.
    """

    def __init__(
        self, form_data: "js.FormData | None | dict[str, FormDataValue]" = None
    ):
        if not form_data:
            self._js_form_data = js.FormData.new()
            return

        if isinstance(form_data, dict):
            self._js_form_data = js.FormData.new()
            for k, v in form_data.items():
                self._js_form_data.append(k, _py_value_to_js(v))
            return

        if (
            hasattr(form_data, "constructor")
            and form_data.constructor.name == "FormData"
        ):
            self._js_form_data = form_data
            return

        raise TypeError("Expected form_data to be a dict or an instance of FormData")

    def __getitem__(self, key: str) -> FormDataValue:
        return _js_value_to_py(self._js_form_data.get(key))

    def __setitem__(self, key: str, value: FormDataValue):
        if isinstance(value, list):
            raise TypeError("Expected single item in arguments to FormData.__setitem__")
        self._js_form_data.set(key, _py_value_to_js(value))

    def append(self, key: str, value: FormDataValue, filename: str | None = None):
        self._js_form_data.append(key, _py_value_to_js(value), filename)

    def delete(self, key: str):
        self._js_form_data.delete(key)

    def __contains__(self, key: str) -> bool:
        return self._js_form_data.has(key)

    def values(self) -> Generator[FormDataValue, None, None]:
        for val in self._js_form_data.values():
            yield _js_value_to_py(val)

    def keys(self) -> Generator[str, None, None]:
        yield from self._js_form_data.keys()

    def __iter__(self):
        yield from self.keys()

    def items(self) -> Generator[tuple[str, FormDataValue], None, None]:
        for k, v in self._js_form_data.entries():
            yield (k, _js_value_to_py(v))

    def __delitem__(self, key: str):
        self.delete(key)

    def __len__(self):
        return len(self.keys())

    def get_all(self, key: str) -> list[FormDataValue]:
        return [_js_value_to_py(x) for x in self._js_form_data.getAll(key)]

    @property
    def js_object(self) -> "js.FormData":
        return self._js_form_data


def _supports_buffer_protocol(o):
    try:
        # memoryview used only for testing type; 'with' releases the view instantly
        with memoryview(o):
            return True
    except TypeError:
        return False


@contextmanager
def _make_blob_entry(e):
    if isinstance(e, str):
        yield e
        return
    if isinstance(e, Blob):
        yield e._js_blob
        return
    if hasattr(e, "constructor") and (e.constructor.name in ("Blob", "File")):
        yield e
        return
    if _supports_buffer_protocol(e):
        px = create_proxy(e)
        buf = px.getBuffer()
        try:
            yield buf.data
            return
        finally:
            buf.release()
            px.destroy()
    raise TypeError(f"Don't know how to handle {type(e)} for Blob()")


def _is_iterable(obj):
    try:
        iter(obj)
    except TypeError:
        return False
    else:
        return True


BlobValue = (
    "str | bytes | js.ArrayBuffer | js.TypedArray | js.DataView | js.Blob | Blob | File"
)


class BlobEnding(StrEnum):
    TRANSPARENT = "transparent"
    NATIVE = "native"


class Blob:
    def __init__(
        self,
        blob_parts: "Iterable[BlobValue] | BlobValue",
        content_type: str | None = None,
        endings: BlobEnding | str | None = None,
    ):
        if endings:
            endings = str(endings)

        is_single_item = not _is_iterable(blob_parts)
        if is_single_item:
            # Inherit the content_type if we have a single item. If a File is passed
            # in then its metadata is lost.
            if not content_type and isinstance(blob_parts, Blob):
                content_type = blob_parts.content_type
            if hasattr(blob_parts, "constructor") and (
                blob_parts.constructor.name in ("Blob", "File")
            ):
                if not content_type:
                    content_type = blob_parts.type

            # Otherwise create a new Blob below.
            blob_parts = [blob_parts]

        with ExitStack() as stack:
            args = [stack.enter_context(_make_blob_entry(e)) for e in blob_parts]
            with _manage_pyproxies() as pyproxies:
                self._js_blob = js.Blob.new(
                    to_js(args, pyproxies=pyproxies),
                    type=content_type,
                    endings=endings,
                )

    @property
    def size(self) -> int:
        return self._js_blob.size

    @property
    def content_type(self) -> str:
        return self._js_blob.type

    @property
    def js_object(self) -> "js.Blob":
        return self._js_blob

    async def text(self) -> str:
        return await self.js_object.text()

    async def bytes(self) -> bytes:
        return (await self.js_object.arrayBuffer()).to_bytes()

    def slice(
        self,
        start: int | None = None,
        end: int | None = None,
        content_type: str | None = None,
    ):
        return self.js_object.slice(start, end, content_type)


class File(Blob):
    def __init__(
        self,
        blob_parts: "Iterable[BlobValue] | BlobValue",
        filename: str,
        content_type: str | None = None,
        endings: BlobEnding | str | None = None,
        last_modified: int | None = None,
    ):
        if endings:
            endings = str(endings)

        is_single_item = not _is_iterable(blob_parts)
        if is_single_item:
            # Inherit the content_type and lastModified if we have a
            # single item.
            if not content_type and isinstance(blob_parts, Blob):
                content_type = blob_parts.content_type
            if not last_modified and isinstance(blob_parts, File):
                last_modified = blob_parts.last_modified
            if hasattr(blob_parts, "constructor") and (
                blob_parts.constructor.name in ("Blob", "File")
            ):
                if not content_type:
                    content_type = blob_parts.type
                if blob_parts.constructor.name == "File":
                    if not last_modified:
                        last_modified = blob_parts.lastModified

            # Otherwise create a new File below.
            blob_parts = [blob_parts]

        with ExitStack() as stack:
            args = [stack.enter_context(_make_blob_entry(e)) for e in blob_parts]
            with _manage_pyproxies() as pyproxies:
                self._js_blob = js.File.new(
                    to_js(args, pyproxies=pyproxies),
                    filename,
                    type=content_type,
                    endings=endings,
                    lastModified=last_modified,
                )

    @property
    def name(self) -> str:
        return self._js_blob.name

    @property
    def last_modified(self) -> int:
        return self._js_blob.last_modified


class Request:
    def __init__(self, input: "Request | str", **other_options: Unpack[FetchKwargs]):
        if "method" in other_options and isinstance(
            other_options["method"], HTTPMethod
        ):
            other_options["method"] = other_options["method"].value

        if "headers" in other_options:
            other_options["headers"] = _to_js_headers(other_options["headers"])
        self._js_request = js.Request.new(
            input._js_request if isinstance(input, Request) else input, **other_options
        )

    @property
    def js_object(self) -> "js.Request":
        return self._js_request

    # TODO: expose `body` as a native Python stream in the future, follow how we define `Response`
    @property
    def body(self) -> "js.ReadableStream":
        return self.js_object.body

    @property
    def body_used(self) -> bool:
        return self.js_object.bodyUsed

    @property
    def cache(self) -> str:
        return self.js_object.cache

    @property
    def credentials(self) -> str:
        return self.js_object.credentials

    @property
    def destination(self) -> str:
        return self.js_object.destination

    @property
    def headers(self) -> http.client.HTTPMessage:
        result = http.client.HTTPMessage()

        for key, val in self.js_object.headers:
            for subval in val.split(","):
                result[key] = subval.strip()

        return result

    @property
    def integrity(self) -> str:
        return self.js_object.integrity

    @property
    def is_history_navigation(self) -> bool:
        return self.js_object.isHistoryNavigation

    @property
    def keepalive(self) -> bool:
        return self.js_object.keepalive

    @property
    def method(self) -> HTTPMethod:
        return HTTPMethod[self.js_object.method]

    @property
    def mode(self) -> str:
        return self.js_object.mode

    @property
    def redirect(self) -> str:
        return self.js_object.redirect

    @property
    def referrer(self) -> str:
        return self.js_object.referrer

    @property
    def referrer_policy(self) -> str:
        return self.js_object.referrerPolicy

    @property
    def url(self) -> str:
        return self.js_object.url

    def _raise_if_failed(self) -> None:
        # TODO: https://github.com/pyodide/pyodide/blob/a53c17fd8/src/py/pyodide/http.py#L252
        if self.body_used:
            # TODO: Use BodyUsedError in newer Pyodide versions.
            raise OSError("Body already used")

    """
    Instance methods defined below.

    The naming of these methods should match Request's methods when possible.

    TODO: AbortController support.
    """

    async def buffer(self) -> "js.ArrayBuffer":
        # The naming of this method matches that of Response.
        self._raise_if_failed()
        return await self.js_object.arrayBuffer()

    async def form_data(self) -> "FormData":
        self._raise_if_failed()
        try:
            return FormData(await self.js_object.formData())
        except JsException as exc:
            raise _to_python_exception(exc) from exc

    async def blob(self) -> Blob:
        self._raise_if_failed()
        return Blob(await self.js_object.blob())

    async def bytes(self) -> bytes:
        self._raise_if_failed()
        return (await self.buffer()).to_bytes()

    def clone(self) -> "Request":
        if self.body_used:
            # TODO: Use BodyUsedError in newer Pyodide versions.
            raise OSError("Body already used")
        return Request(
            self.js_object.clone(),
        )

    async def json(self, **kwargs: Any) -> Any:
        self._raise_if_failed()
        return json.loads(await self.text(), **kwargs)

    async def text(self) -> str:
        self._raise_if_failed()
        return await self.js_object.text()


def handler(func):
    """
    When applied to handlers such as `on_fetch` it will rewrite arguments passed in to native Python
    types defined in this module. For example, the `request` argument to `on_fetch` gets converted
    to an instance of the Request class defined in this module.
    """

    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        # TODO: wrap `env` so that bindings can be used without to_js.
        if (
            len(args) > 0
            and hasattr(args[0], "constructor")
            and args[0].constructor.name == "Request"
        ):
            args = (Request(args[0]),) + args[1:]
        return func(*args, **kwargs)

    return wrapper


class DurableObject:
    """
    Base class used to define a Durable Object.
    """

    pass
