# This module defines a Workers API for Python. It is similar to the API provided by
# JS Workers, but with changes and additions to be more idiomatic to the Python
# programming language.
from collections.abc import Generator, MutableMapping
from http import HTTPMethod, HTTPStatus
from typing import TypedDict, Unpack

import js

import pyodide.http
from pyodide.ffi import JsException, to_js
from pyodide.http import pyfetch

JSBody = (
    "js.Blob | js.ArrayBuffer | js.TypedArray | js.DataView | js.FormData |"
    "js.ReadableStream | js.URLSearchParams"
)
Body = "str | FormData | JSBody"
Headers = dict[str, str] | list[tuple[str, str]]


class FetchKwargs(TypedDict, total=False):
    headers: Headers | None
    body: "Body | None"
    method: HTTPMethod = HTTPMethod.GET


class FetchResponse(pyodide.http.FetchResponse):
    # TODO: Consider upstreaming the `body` attribute
    @property
    def body(self) -> Body:
        """
        Returns the body from the JavaScript Response instance.
        """
        b = self.js_response.body
        if b.constructor.name == "FormData":
            return FormData(b)
        else:
            return b

    """
    Instance methods defined below.

    Some methods are implemented by `FetchResponse`, these include `buffer`
    (replacing JavaScript's `arrayBuffer`), `bytes`, `json`, and `text`.

    Some methods are intentionally not implemented, these include `blob`.

    There are also some additional methods implemented by `FetchResponse`.
    See https://pyodide.org/en/stable/usage/api/python-api/http.html#pyodide.http.FetchResponse
    for details.
    """

    async def formData(self) -> "FormData":
        try:
            return FormData(await self.js_response.formData())
        except JsException as exc:
            raise _to_python_exception(exc) from exc


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


class Response(FetchResponse):
    def __init__(
        self,
        body: Body,
        status: HTTPStatus | int = HTTPStatus.OK,
        statusText="",
        headers: Headers = None,
    ):
        """
        Represents the response to a request.

        Based on the JS API of the same name:
        https://developer.mozilla.org/en-US/docs/Web/API/Response/Response.
        """
        options = self._create_options(status, statusText, headers)

        # Initialise via the FetchResponse super-class which gives us access to
        # methods that we would ordinarily have to redeclare.
        js_resp = js.Response.new(
            body.js_form_data if isinstance(body, FormData) else body, **options
        )
        super().__init__(js_resp.url, js_resp)

    @staticmethod
    def _create_options(
        status: HTTPStatus | int = HTTPStatus.OK, statusText="", headers: Headers = None
    ):
        options = {
            "status": status.value if isinstance(status, HTTPStatus) else status,
        }
        if len(statusText) > 0:
            options["statusText"] = statusText
        if headers:
            if isinstance(headers, list):
                # We should have a list[tuple[str, str]]
                options["headers"] = js.Headers.new(headers)
            elif isinstance(headers, dict):
                options["headers"] = js.Headers.new(headers.items())
            else:
                raise TypeError("Received unexpected type for headers argument")

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
        statusText="",
        headers: Headers = None,
    ):
        options = Response._create_options(status, statusText, headers)
        try:
            return js.Response.json(
                to_js(data, dict_converter=js.Object.fromEntries), **options
            )
        except JsException as exc:
            raise _to_python_exception(exc) from exc


# TODO: Implement pythonic blob API
FormDataValue = "str | js.Blob"


class FormData(MutableMapping[str, FormDataValue]):
    def __init__(self, form_data: "js.FormData | None | dict[str, FormDataValue]"):
        if form_data:
            if isinstance(form_data, dict):
                self.js_form_data = js.FormData.new()
                for item in form_data.items():
                    self.js_form_data.append(item[0], item[1])
            else:
                self.js_form_data = form_data
        else:
            self.js_form_data = js.FormData.new()

    def __getitem__(self, key: str) -> list[FormDataValue]:
        return list(self.js_form_data.getAll(key))

    def __setitem__(self, key: str, value: list[FormDataValue]):
        self.js_form_data.delete(key)
        for item in value:
            self.js_form_data.append(key, item)

    def append(self, key: str, value: FormDataValue):
        self.js_form_data.append(key, value)

    def delete(self, key: str):
        self.js_form_data.delete(key)

    def __contains__(self, key: str) -> bool:
        return self.js_form_data.has(key)

    def values(self) -> Generator[FormDataValue, None, None]:
        yield from self.js_form_data.values()

    def keys(self) -> Generator[str, None, None]:
        yield from self.js_form_data.keys()

    def __iter__(self):
        yield from self.keys()

    def items(self) -> Generator[tuple[str, FormDataValue], None, None]:
        for item in self.js_form_data.entries():
            yield (item[0], item[1])

    def __delitem__(self, key: str):
        self.delete(key)

    def __len__(self):
        return len(self.keys())
