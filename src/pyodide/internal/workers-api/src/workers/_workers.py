# This module defines a Workers API for Python. It is similar to the API provided by
# JS Workers, but with changes and additions to be more idiomatic to the Python
# programming language.
import datetime
import functools
import inspect
import json
from asyncio import create_task, gather
from collections.abc import Awaitable, Generator, Iterable, MutableMapping
from contextlib import ExitStack, contextmanager
from enum import StrEnum
from http import HTTPMethod, HTTPStatus
from types import LambdaType
from typing import Any, Never, Protocol, TypedDict, Unpack

# Get globals modules and import function from the entrypoint-helper
import _pyodide_entrypoint_helper
import js
from js import Object

import pyodide.http
from pyodide import __version__ as pyodide_version
from pyodide.ffi import (
    JsBuffer,
    JsException,
    JsProxy,
    create_proxy,
    destroy_proxies,
    to_js,
)
from pyodide.http import pyfetch
from workers.workflows import NonRetryableError


class Context(Protocol):
    def waitUntil(self, other: Awaitable[Any]) -> None: ...


try:
    from pyodide.ffi import jsnull
except ImportError:
    jsnull = None


def _jsnull_to_none(x):
    if x is jsnull:
        return None
    return x


def import_from_javascript(module_name: str) -> Any:
    """
    Import a JavaScript ES module from Python.

    Args:
        module_name: The name of the module to import. This can be a module name or a path.

    Returns:
        The imported module object.

    Example:
        cloudflare_workers = import_from_javascript("cloudflare:workers")
        env = cloudflare_workers.env

    Note:
        Behind the scenes import_from_javascript uses JSPI to do imports but that means we need an
        async context. To enable importing cloudflare:workers and cloudflare:sockets in the global
        scope we specifically imported them in the global scope and exposed them here.
    """
    # Special case for global scope available modules
    # JSPI won't work in the global scope in 0.26.0a2 so we need modules importable in the global
    # scope to be imported beforehand.
    if module_name == "cloudflare:workers":
        return _pyodide_entrypoint_helper.cloudflareWorkersModule
    elif module_name == "cloudflare:sockets":
        return _pyodide_entrypoint_helper.cloudflareSocketsModule

    try:
        from pyodide.ffi import run_sync

        # Call the JavaScript import function
        return run_sync(_pyodide_entrypoint_helper.doAnImport(module_name))
    except JsException as e:
        raise ImportError(f"Failed to import '{module_name}': {e}") from e
    except RuntimeError as e:
        if e.args[0] == "No suspender":
            raise ImportError(
                f"Failed to import '{module_name}': Only 'cloudflare:workers' and 'cloudflare:sockets' are available in the global scope."
            ) from e
        raise
    except ImportError as e:
        if e.args[0].startswith("cannot import name 'run_sync' from 'pyodide.ffi'"):
            raise ImportError(
                f"Failed to import '{module_name}': Only 'cloudflare:workers' and 'cloudflare:sockets' are available until the next python runtime version."
            ) from e
        raise


type JSBody = (
    "js.Blob | JsBuffer | js.FormData | js.ReadableStream | js.URLSearchParams"
)
type Body = "str | FormData | JSBody"
type Headers = "dict[str, str] | list[tuple[str, str]] | js.Headers"


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
    method: HTTPMethod | None
    redirect: str | None
    cf: RequestInitCfProperties | None
    fetcher: type[pyfetch] | None


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
        return _jsnull_to_none(self.js_response.body)

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

    def replace_body(self, body: Body) -> "Response":
        """
        Returns a new Response object with the same options (status, headers, etc) as
        the original but with an updated body.
        """
        b = body.js_object if isinstance(body, FormData) else body
        js_resp = js.Response.new(b, self.js_response)
        return Response(js_resp)

    async def blob(self) -> "Blob":
        self._raise_if_failed()
        return Blob(await self.js_object.blob())

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
    def from_json(
        data: str | dict[str, Any] | list[Any] | JsProxy,
        status: HTTPStatus | int = HTTPStatus.OK,
        status_text="",
        headers: Headers = None,
    ) -> "Response":
        options = Response._create_options(status, status_text, headers)
        js_resp = None
        try:
            if isinstance(data, JsProxy):
                js_resp = js.Response.json(data, **options)
            else:
                if "headers" not in options:
                    options["headers"] = _to_js_headers(
                        {"content-type": "application/json"}
                    )
                elif not options["headers"].has("content-type"):
                    options["headers"].set("content-type", "application/json")
                js_resp = js.Response.new(json.dumps(data), **options)
        except JsException as exc:
            raise _to_python_exception(exc) from exc

        return Response(js_resp)

    def json(self, *args: Never, **kwargs: Never):
        if isinstance(self, Response):
            return super().json()
        # For compatibility, allow static use of Response.json() to mean Response.from_json().
        data = self
        return Response.from_json(data, *args, **kwargs)


if pyodide_version == "0.26.0a2":

    async def _pyfetch_patched(
        request: "str | js.Request", **kwargs: Any
    ) -> "Response":
        # This is copied from https://github.com/pyodide/pyodide/blob/d3f99e1d/src/py/pyodide/http.py
        custom_fetch = kwargs["fetcher"] if "fetcher" in kwargs else js.fetch
        kwargs["fetcher"] = None
        try:
            return Response(
                await custom_fetch(
                    request, to_js(kwargs, dict_converter=Object.fromEntries)
                ),
            )
        except JsException as e:
            raise OSError(e.message) from None
else:
    _pyfetch_patched = pyfetch


async def fetch(
    resource: "str | Request | js.Request",
    **other_options: Unpack[FetchKwargs],
) -> "Response":
    if isinstance(resource, Request):
        resource = resource.js_object
    if "method" in other_options and isinstance(other_options["method"], HTTPMethod):
        other_options["method"] = other_options["method"].value
    resp = await _pyfetch_patched(resource, **other_options)
    return Response(resp.js_response)


def _to_python_exception(exc: JsException) -> Exception:
    if exc.name == "RangeError":
        return ValueError(exc.message)
    elif exc.name == "TypeError":
        return TypeError(exc.message)
    else:
        return exc


def _from_js_error(exc: JsException) -> Exception:
    # convert into Python exception after a full round trip
    # Python - JS - Python
    if not exc.message or not exc.message.startswith("PythonError"):
        return _to_python_exception(exc)

    # extract the Python exception type from the traceback
    error_message_last_line = exc.message.split("\n")[-2]
    if error_message_last_line.startswith("TypeError"):
        return TypeError(error_message_last_line)
    elif error_message_last_line.startswith("ValueError"):
        return ValueError(error_message_last_line)
    elif error_message_last_line.startswith("workers.workflows.NonRetryableError"):
        return NonRetryableError(error_message_last_line)
    else:
        return _to_python_exception(exc)


@contextmanager
def _manage_pyproxies():
    proxies = js.Array.new()
    try:
        yield proxies
    finally:
        destroy_proxies(proxies)


def _is_js_instance(val, js_cls_name):
    return hasattr(val, "constructor") and val.constructor.name == js_cls_name


def _to_js_headers(headers: Headers):
    if isinstance(headers, list):
        # We should have a list[tuple[str, str]]
        return js.Headers.new(headers)
    elif isinstance(headers, dict):
        return js.Headers.new(headers.items())
    elif _is_js_instance(headers, "Headers"):
        return headers
    else:
        raise TypeError("Received unexpected type for headers argument")


class Response(FetchResponse):
    """
    This class represents the response to an HTTP request, with a similar API to that of the web
    `Response` API: https://developer.mozilla.org/en-US/docs/Web/API/Response.
    """

    def __init__(
        self,
        body: Body = None,
        status: HTTPStatus | int | None = None,
        status_text="",
        headers: Headers = None,
        web_socket: "js.WebSocket | None" = None,
    ):
        """
        Represents the response to a request.

        Based on the JS API of the same name:
        https://developer.mozilla.org/en-US/docs/Web/API/Response/Response.
        """
        # Verify passed in types.
        if hasattr(body, "constructor"):
            if body.constructor.name not in (
                "Blob",
                "ArrayBuffer",
                "TypedArray",
                "DataView",
                "FormData",
                "ReadableStream",
                "URLSearchParams",
                "Response",
            ):
                raise TypeError(
                    f"Unsupported type in Response: {body.constructor.name}"
                )
        elif not isinstance(body, str | FormData) and body is not None:
            raise TypeError(f"Unsupported type in Response: {type(body).__name__}")

        # Handle constructing a Response from a JS Response.
        if _is_js_instance(body, "Response"):
            if status is not None or len(status_text) > 0 or headers is not None:
                raise ValueError(
                    "Expected no options when constructing Response from a js.Response"
                )
            super().__init__(body.url, body)
            return

        options = self._create_options(status, status_text, headers)

        # Initialize via the FetchResponse super-class which gives us access to
        # methods that we would ordinarily have to redeclare.
        js_resp = js.Response.new(
            body.js_object if isinstance(body, FormData) else body, **options
        )
        super().__init__(js_resp.url, js_resp)

    def __repr__(self):
        body = [f"status={self.status}"]
        if self.js_object.statusText:
            body.append(f"status_text={self.status_text!r}")
        if "content-type" in self.headers:
            body.append(f"content_type={self.headers['content-type']!r}")
        if self.js_object.url:
            body.append(f"url={self.js_object.url!r}")
        if self.js_object.type != "default":
            body.append(f"type={self.js_object.type!r}")
        return f"Response({', '.join(body)})"

    @staticmethod
    def _create_options(
        status: HTTPStatus | int | None = HTTPStatus.OK,
        status_text="",
        headers: Headers = None,
        web_socket: "js.WebSocket | None" = None,
    ):
        options = {}
        if status:
            options["status"] = (
                status.value if isinstance(status, HTTPStatus) else status
            )
        if status_text:
            options["statusText"] = status_text
        if headers:
            options["headers"] = _to_js_headers(headers)
        if web_socket:
            options["webSocket"] = web_socket
        return options


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
    This class represents a set of key/value pairs for forms.

    The API of this class follows that of https://pypi.org/project/multidict/ and
    https://developer.mozilla.org/en-US/docs/Web/API/FormData.
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

        if _is_js_instance(form_data, "FormData"):
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
    if isinstance(obj, (str, bytes)):
        return False
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
        js_sliced_blob = self.js_object.slice(start, end, content_type)
        return Blob([js_sliced_blob])


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
        return self._js_blob.lastModified


class Request:
    def __init__(
        self, input: "Request | str | js.Request", **other_options: Unpack[FetchKwargs]
    ):
        if _is_js_instance(input, "Request"):
            if len(other_options) > 0:
                raise ValueError(
                    "Expected no options when constructing Request from a js.Request"
                )
            self._js_request = input
            return

        if "method" in other_options and isinstance(
            other_options["method"], HTTPMethod
        ):
            other_options["method"] = other_options["method"].value

        if "headers" in other_options:
            other_options["headers"] = _to_js_headers(other_options["headers"])
        self._js_request = js.Request.new(
            input._js_request if isinstance(input, Request) else input, **other_options
        )

    def __repr__(self):
        return (
            f"Request(method={self._js_request.method!r}, url={self._js_request.url!r})"
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
    def headers(self):
        # This is imported here because it costs a lot of CPU time when imported at the top-level.
        # At least it does when we do so in our validator tests, doesn't seem to cause trouble in
        # production. So as a workaround we do the import here.
        #
        # TODO(later): when dedicated snapshots are default we can move this import to the top-level.
        import http.client

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


def _python_from_rpc_default_converter(value, convert, cache):
    if not hasattr(value, "constructor"):
        # Assume that the object doesn't need conversion as it's not a JS object.
        return value

    if value.constructor.name == "Response":
        return Response(value)
    elif value.constructor.name == "FormData":
        return FormData(value)
    elif value.constructor.name == "Blob":
        return Blob(value)
    elif value.constructor.name == "File":
        return File(value)
    elif value.constructor.name == "Request":
        return Request(value)
    elif value.constructor.name == "Date":
        # TODO: Pyodide should gain support for this, we should upstream this.
        return datetime.datetime.fromtimestamp(value.getTime() / 1000)
    elif value.constructor.name == "Error":
        return Exception(value.toString())
    elif value.constructor.name == "Number":
        return value.valueOf()

    raise TypeError(
        f"Couldn't convert object to Python type, got {value.constructor.name}"
    )


def python_from_rpc(obj: "JsProxy"):
    """
    Converts JS objects like Response, Request, Blob, etc. to equivalent Python objects defined in
    this module and also other JS objects like Map, Set, etc. to equivalent Python stdlib objects.

    This method is used for Workers RPC in Python to convert JavaScript objects to Python. As such
    it does not support serializing all JS object types.
    """

    if not hasattr(obj, "constructor"):
        return obj

    if obj.constructor.name == "TestController":
        # This object currently has no methods defined on it. If this changes we should
        # implement a Python wrapper for it, but for now we'll just pass in None.
        return None

    result = obj.to_py(default_converter=_python_from_rpc_default_converter)

    return result


def _raise_on_disabled_type(value):
    if _is_js_instance(value, "RegExp"):
        raise TypeError(f"{value.constructor.name} cannot be sent over RPC.")

    if isinstance(value, (tuple, bytearray, LambdaType)):
        raise TypeError(f"{type(value)} cannot be sent over RPC.")

    if _is_iterable(value):
        if isinstance(value, dict):
            for v in value.values():
                _raise_on_disabled_type(v)
        else:
            for v in value:
                _raise_on_disabled_type(v)


def _python_to_rpc_default_converter(obj, convert, cache):
    if obj is None:
        return obj

    if hasattr(obj, "js_object"):
        return obj.js_object

    if isinstance(obj, datetime.datetime):
        # TODO: Pyodide should gain support for this, we should upstream this.
        return js.Date.new(obj.timestamp() * 1000)

    if isinstance(obj, Exception):
        return js.Error.new(str(obj))

    _raise_on_disabled_type(obj)

    return obj


def python_to_rpc(value) -> JsProxy:
    """
    Converts Python objects defined in this module (Response, Request, etc) and native Python types
    like Map, Set, datetime to equivalent JavaScript types.

    This method is used for Workers RPC in Python to convert Python objects to JavaScript. As such
    it does not support serializing all Python object types.
    """

    # `to_js` won't always call the default_converter, for example when a list of tuples is passed
    _raise_on_disabled_type(value)

    result = to_js(
        value,
        default_converter=_python_to_rpc_default_converter,
        dict_converter=js.Map.new,
    )

    return result


class _FetcherWrapper:
    def __init__(self, binding):
        self._binding = binding

    def _getattr_helper(self, name):
        attr = getattr(self._binding, name)

        if not callable(attr):
            return attr

        # Not using `@functools.wraps(attr)` here because `attr` is a JS proxy.
        async def wrapper(*args, **kwargs):
            js_args = [python_to_rpc(arg) for arg in args]
            js_kwargs = {k: python_to_rpc(v) for k, v in kwargs.items()}
            result = attr(*js_args, **js_kwargs)
            if hasattr(result, "then") and callable(result.then):
                return python_from_rpc(await result)
            else:
                return python_from_rpc(result)

        return wrapper

    def __getattr__(self, name):
        result = self._getattr_helper(name)
        setattr(self, name, result)
        return result

    def fetch(self, *args, **kwargs):
        return fetch(*args, fetcher=self._binding.fetch, **kwargs)


class _DurableObjectNamespaceWrapper:
    def __init__(self, binding):
        self._binding = binding

    def __getattr__(self, name):
        return getattr(self._binding, name)

    def get(self, *args, **kwargs):
        return _FetcherWrapper(self._binding.get(*args, **kwargs))


class _EnvWrapper:
    def __init__(self, env: Any):
        self._env = env

    def _getattr_helper(self, name):
        binding = getattr(self._env, name)
        if _is_js_instance(binding, "Fetcher"):
            return _FetcherWrapper(binding)

        if _is_js_instance(binding, "DurableObjectNamespace"):
            return _DurableObjectNamespaceWrapper(binding)

        # TODO: Implement APIs for bindings.
        return binding

    def __getattr__(self, name):
        result = self._getattr_helper(name)
        setattr(self, name, result)
        return result


def handler(func):
    """
    When applied to handlers such as `on_fetch` it will rewrite arguments passed in to native Python
    types defined in this module. For example, the `request` argument to `on_fetch` gets converted
    to an instance of the Request class defined in this module.
    """

    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        # TODO: support transforming kwargs
        if len(args) > 0 and _is_js_instance(args[0], "Request"):
            args = (Request(args[0]), *args[1:])

        # Wrap `env` so that bindings can be used without to_js.
        if len(args) > 1:
            args = (args[0], _EnvWrapper(args[1]), *args[2:])

        return func(*args, **kwargs)

    return wrapper


class _WorkflowStepWrapper:
    def __init__(self, js_step):
        self._js_step = js_step
        self._memoized_dependencies = {}
        self._in_flight = {}

    def do(self, name, depends=None, concurrent=False, config=None):
        def decorator(func):
            async def wrapper():
                if concurrent:
                    results = await gather(
                        *[self._resolve_dependency(dep) for dep in depends or []]
                    )
                else:
                    results = [
                        await self._resolve_dependency(dep) for dep in depends or []
                    ]
                python_results = [python_from_rpc(result) for result in results]
                return await _do_call(self, name, config, func, *python_results)

            wrapper._step_name = name
            return wrapper

        return decorator

    def sleep(self, *args, **kwargs):
        # all types should be primitives - no need for explicit translation
        return self._js_step.sleep(*args, **kwargs)

    def sleep_until(self, name, timestamp):
        if not isinstance(timestamp, str):
            timestamp = python_to_rpc(timestamp)

        return self._js_step.sleepUntil(name, timestamp)

    def wait_for_event(self, name, event_type, /, timeout="24 hours"):
        return self._js_step.waitForEvent(
            name,
            to_js(
                {"type": event_type, "timeout": timeout},
                dict_converter=Object.fromEntries,
            ),
        )

    async def _resolve_dependency(self, dep):
        if dep._step_name in self._memoized_dependencies:
            return self._memoized_dependencies[dep._step_name]
        elif dep._step_name in self._in_flight:
            return await self._in_flight[dep._step_name]

        return await dep()


async def _do_call(entrypoint, name, config, callback, *results):
    async def _callback():
        result = callback(*results)

        if inspect.iscoroutine(result):
            result = await result
        return to_js(result, dict_converter=Object.fromEntries)

    async def _closure():
        try:
            if config is None:
                coroutine = await entrypoint._js_step.do(name, _callback)
            else:
                coroutine = await entrypoint._js_step.do(
                    name, to_js(config, dict_converter=Object.fromEntries), _callback
                )

            return python_from_rpc(coroutine)
        except Exception as exc:
            raise _from_js_error(exc) from exc

    task = create_task(_closure())
    entrypoint._in_flight[name] = task

    try:
        result = await task
        entrypoint._memoized_dependencies[name] = result
    finally:
        del entrypoint._in_flight[name]

    return result


def _wrap_subclass(cls):
    # Override the class __init__ so that we can wrap the `env` in the constructor.
    original_init = cls.__init__

    def wrapped_init(self, *args, **kwargs):
        if len(args) > 0:
            _pyodide_entrypoint_helper.patchWaitUntil(args[0])
        if len(args) > 1:
            args = list(args)
            args[1] = _EnvWrapper(args[1])

        original_init(self, *args, **kwargs)

    cls.__init__ = wrapped_init


def _wrap_workflow_step(cls):
    run_fn = getattr(cls, "run", None)
    if run_fn is None:
        return

    # Only patch `on_run` for subclasses of WorkflowEntrypoint.
    if not issubclass(cls, WorkflowEntrypoint):
        # Not a workflow subclass, so don't wrap `on_run`.
        return

    @functools.wraps(run_fn)
    async def wrapped_run(self, event=None, step=None, /, *args, **kwargs):
        if event is not None:
            event = python_from_rpc(event)
        if step is not None:
            step = _WorkflowStepWrapper(step)

        result = run_fn(self, event, step, *args, **kwargs)

        if inspect.iscoroutine(result):
            result = await result

        return result

    cls.run = wrapped_run


class DurableObject:
    """
    Base class used to define a Durable Object.
    """

    def __init__(self, ctx: Context, env: Any):
        self.ctx = ctx
        self.env = env

    def __init_subclass__(cls, **_kwargs):
        _wrap_subclass(cls)


class WorkerEntrypoint:
    """
    Base class used to define a Worker Entrypoint.
    """

    def __init__(self, ctx: Context, env: Any):
        self.ctx = ctx
        self.env = env

    def __init_subclass__(cls, **_kwargs: Any):
        _wrap_subclass(cls)


class WorkflowEntrypoint:
    """
    Base class used to define a Workflow Entrypoint.
    """

    def __init__(self, ctx: Context, env: Any):
        self.ctx = ctx
        self.env = env

    def __init_subclass__(cls, **_kwargs: Any):
        _wrap_subclass(cls)
        _wrap_workflow_step(cls)
