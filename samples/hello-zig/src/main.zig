const std = @import("std");

//////////////
/// Types
pub const cloudflare_string_t = extern struct {
    ptr: [*c]const u8,
    len: usize,

    fn from_slice(slice: []const u8) cloudflare_string_t {
        return cloudflare_string_t{ .ptr = slice.ptr, .len = slice.len };
    }

    fn as_slice(self: cloudflare_string_t) []const u8 {
        return self.ptr[0..self.len];
    }
};

const http_tuple2_string_string_t = extern struct { f0: cloudflare_string_t, f1: cloudflare_string_t };

const http_list_tuple2_string_string_t = extern struct {
    ptr: [*c]http_tuple2_string_string_t,

    len: usize,

    fn from_slice(slice: []http_tuple2_string_string_t) http_list_tuple2_string_string_t {
        return http_list_tuple2_string_string_t{ .ptr = slice.ptr, .len = slice.len };
    }

    fn as_slice(self: http_list_tuple2_string_string_t) []http_tuple2_string_string_t {
        return self.ptr[0..self.len];
    }
};

const http_request_t = extern struct {
    url: cloudflare_string_t,
    headers: http_list_tuple2_string_string_t,
    body: cloudflare_string_t,
};

const http_response_t = extern struct {
    status: u16,
    headers: http_list_tuple2_string_string_t,
    body: cloudflare_string_t,
};

const http_response_handle_t = u32;

//////// Imports

extern fn console_log(str: cloudflare_string_t) void;
extern fn http_fetch(req: *http_request_t) http_response_handle_t;
extern fn http_resolve_response(response: http_response_handle_t, ret: *http_response_t) bool;

//////// Exports

const root_allocator = std.heap.wasm_allocator;

export fn alloc(size: u32) ?[*]u8 {
    const mem = root_allocator.alloc(u8, size) catch return @intToPtr(?[*]u8, 0);
    return mem.ptr;
}

export fn free(ptr: [*]u8, size: u32) void {
    root_allocator.free(ptr[0..size]);
}

const worker_request_t = http_request_t;
const worker_response_t = http_response_t;

export fn worker_fetch(request: *worker_request_t, response: *worker_response_t) void {
    hello_world_worker_fetch(request, response);
}

//////// Implementation

fn hello_world_worker_fetch(request: *worker_request_t, response: *worker_response_t) void {
    _ = request;
    response.body = cloudflare_string_t.from_slice("Hello World\n");
}

fn demo_worker_fetch(request: *worker_request_t, response: *worker_response_t) void {
    demo_worker_fetch_impl(request, response) catch |err| {
        const code: u16 = switch (err) {
            error.OutOfMemory => 1,
            error.InvalidFormat => 2,
            error.UnexpectedCharacter => 3,
            error.InvalidPort => 4,
        };
        response.status = 500 + code;
    };
}

fn demo_worker_fetch_impl(request: *worker_request_t, response: *worker_response_t) !void {
    console_log(cloudflare_string_t.from_slice("enter: worker_fetch_impl"));
    defer console_log(cloudflare_string_t.from_slice("exit: worker_fetch_impl"));

    // const request_allocator = root_allocator;
    var arena = std.heap.ArenaAllocator.init(root_allocator);
    defer arena.deinit();
    const request_allocator = arena.allocator();

    console_log(cloudflare_string_t.from_slice(try std.fmt.allocPrint(request_allocator, "url: {s}", .{request.url.as_slice()})));

    for (request.headers.as_slice()) |header| {
        console_log(cloudflare_string_t.from_slice(try std.fmt.allocPrint(request_allocator, "{s}: {s}", .{ header.f0.as_slice(), header.f1.as_slice() })));
    }

    //  let domain = new URL(request.url).searchParams.get('domain')
    const uri = try std.Uri.parse(request.url.as_slice());

    if (uri.query) |query| {
        const domain = query;
        console_log(cloudflare_string_t.from_slice(try std.fmt.allocPrint(request_allocator, "domain: {s}", .{domain})));
        response.status = 400;
        //     const url = try std.fmt.allocPrint(request_allocator, "http://{s}", .{domain});

        //     var sub_request = http_request_t{
        //         .url = cloudflare_string_t.from_slice(url),
        //         .headers = http_list_tuple2_string_string_t.from_slice(
        //             &[_]http_tuple2_string_string_t{
        //                 // headers: {'User-Agent': request.headers.get('User-Agent')}
        //             },
        //         ),
        //         .body = cloudflare_string_t.from_slice(""),
        //     };
        //     const resp = http_fetch(&sub_request);
        //     _ = resp;

        //     // let headers = new Headers({
        //     //    'Content-Type': 'text/html',
        //     //    'Access-Control-Allow-Origin': '*'
        //     //  })
        //     //  const SECURE_RESPONSE = new Response('secure', {status: 200, headers: headers})
        //     //  const INSECURE_RESPONSE = new Response('not secure', {status: 200, headers: headers})
        //     //  const NO_SUCH_SITE = new Response('website not found', {status: 200, headers: headers})

        //     //  try {
        //     //    if(resp.redirected == true && resp.url.startsWith('https')) {
        //     //      return SECURE_RESPONSE
        //     //    }
        //     //    else if(resp.redirected == false && resp.status == 502) {
        //     //      return NO_SUCH_SITE
        //     //    }
        //     //    else {
        //     //      return INSECURE_RESPONSE
        //     //    }
        //     //  }
        //     //   catch (e) {
        //     //    return new Response(`Something went wrong ${e}`, {status: 404})
        //     //  }

    } else {
        response.status = 404;
        response.body = cloudflare_string_t.from_slice("Please pass in domain via query string");
    }
}
