# This module defines a Workers API for Python. It is similar to the API provided by
# JS Workers, but with changes and additions to be more idiomatic to the Python
# programming language.
from http import HTTPMethod, HTTPStatus

import js

from pyodide.ffi import to_js
from pyodide.http import FetchResponse, pyfetch

JSBody = (
    "js.Blob | js.ArrayBuffer | js.TypedArray | js.DataView | js.FormData |"
    "js.ReadableStream | js.URLSearchParams"
)
Body = "str | JSBody"
Headers = dict[str, str] | list[tuple[str, str]]


async def fetch(
    resource: str,
    headers: Headers = None,
    body: Body | None = None,
    method: HTTPMethod = HTTPMethod.GET,
    **other_options,
) -> FetchResponse:
    if headers:
        other_options["headers"] = headers

    if body:
        other_options["body"] = body

    other_options["method"] = method.value
    other_options["url"] = resource

    return await pyfetch(**other_options)


def Response(
    body: Body, status=HTTPStatus.OK, statusText="", headers: Headers = None
) -> js.Response:
    """
    Represents the response to a request.

    Based on the JS API of the same name:
    https://developer.mozilla.org/en-US/docs/Web/API/Response/Response.
    """
    options = to_js(
        {
            "status": status.value,
            "statusText": statusText,
            "headers": headers if headers else {},
        }
    )
    return js.Response.new(body, options)
