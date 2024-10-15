# This module defines a Workers API for Python. It is similar to the API provided by
# JS Workers, but with changes and additions to be more idiomatic to the Python
# programming language.
from http import HTTPMethod, HTTPStatus
from typing import TypedDict, Unpack

import js

import pyodide.http
from pyodide.http import pyfetch

JSBody = (
    "js.Blob | js.ArrayBuffer | js.TypedArray | js.DataView | js.FormData |"
    "js.ReadableStream | js.URLSearchParams"
)
Body = "str | JSBody"
Headers = dict[str, str] | list[tuple[str, str]]


class FetchKwargs(TypedDict, total=False):
    headers: Headers | None
    body: "Body | None"
    method: HTTPMethod = HTTPMethod.GET


# TODO: This is just here to make the `body` field available as many Workers
# examples which rewrite responses make use of it. It's something we should
# likely upstream to Pyodide.
class FetchResponse(pyodide.http.FetchResponse):
    @property
    def body(self) -> Body:
        """
        Returns the body from the JavaScript Response instance.
        """
        return self.js_response.body


async def fetch(
    resource: str,
    **other_options: Unpack[FetchKwargs],
) -> FetchResponse:
    if "method" in other_options and isinstance(other_options["method"], HTTPMethod):
        other_options["method"] = other_options["method"].value
    resp = await pyfetch(resource, **other_options)
    return FetchResponse(resp.url, resp.js_response)


class Response:
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
                raise TypeError(
                    "Response() received unexpected type for headers argument"
                )

        self.js_response = js.Response.new(body, **options)
