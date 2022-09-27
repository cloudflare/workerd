// An example Worker that serves static files from disk. This includes logic to do things like
// set Content-Type based on file extension, look for `index.html` in directories, etc.
//
// This code supports several configuration options to control the serving logic, but, better
// yet, because it's just JavaScript, you can freely edit it to suit your unique needs.

export default {
  async fetch(req, env) {
    if (req.method != "GET" && req.method != "HEAD") {
      return new Response("Not Implemented", {status: 501});
    }

    let url = new URL(req.url);
    let path = url.pathname;
    let origPath = path;

    let config = env.config || {};

    if (path.endsWith("/") && !config.allowDirectoryListing) {
      path = path + "index.html";
    }

    let content = await env.files.fetch("http://dummy" + path, {method: req.method});

    if (content.status == 404) {
      if (config.hideHtmlExtension && !path.endsWith(".html")) {
        // Try with the `.html` extension.
        path = path + ".html";
        content = await env.files.fetch("http://dummy" + path, {method: req.method});
      }

      if (!content.ok && config.singlePageApp) {
        // For any file not found, serve the main page -- NOT as a 404.
        path = "/index.html";
        content = await env.files.fetch("http://dummy" + path, {method: req.method});
      }

      if (!content.ok) {
        // None of the fallbacks worked.
        //
        // Look for a 404 page.
        content = await env.files.fetch("http://dummy/404.html", {method: req.method});

        if (content.ok) {
          // Return it with a 404 status code.
          return wrapContent(req, 404, "404.html", content.body, content.headers);
        } else {
          // Give up and return generic 404 message.
          return new Response("404 Not found", {status: 404});
        }
      }
    }

    if (!content.ok) {
      // Some error other than 404?
      console.error("Fetching '" + path + "' returned unexpected status: " + content.status);
      return new Response("Internal Server Error", {status: 500});
    }

    if (content.headers.get("Content-Type") == "application/json") {
      // This is a directory.
      if (path.endsWith("/")) {
        // This must be because `allowDirectoryListing` is `true`, so this is actually OK!
        let listingHtml = null;
        if (req.method != "HEAD") {
          let html = await makeListingHtml(origPath, await content.json(), env.files);
          return wrapContent(req, 200, "listing.html", html);
        }
      } else {
        // redirect to add '/' suffix.
        url.pathname = path + "/";
        return Response.redirect(url);
      }
    }

    if (origPath.endsWith("/index.html")) {
      // The file exists, but was requested as "index.html", which we want to hide, so redirect
      // to remove it.
      url.pathname = origPath.slice(0, -"index.html".length);
      return Response.redirect(url);
    }

    if (config.hideHtmlExtension && origPath.endsWith(".html")) {
      // The file exists, but was requested with the `.html` extension, which we want to hide, so
      // redirect to remove it.
      url.pathname = origPath.slice(0, -".html".length);
      return Response.redirect(url);
    }

    return wrapContent(req, 200, path.split("/").pop(), content.body, content.headers);
  }
}

function wrapContent(req, status, filename, contentBody, contentHeaders) {
  let type = TYPES[filename.split(".").pop().toLowerCase()] || "application/octet-stream";
  let headers = { "Content-Type": type };
  if (type.endsWith(";charset=utf-8")) {
    let accept = req.headers.get("Accept-Encoding") || "";
    if (accept.split(",").map(s => s.trim()).includes("gzip")) {
      // Apply gzip encoding on the fly.
      // TODO(someday): Support storing gziped copies of files on disk in advance so that gzip
      //   doesn't need to be applied on the fly.
      headers["Content-Encoding"] = "gzip";
    }
  }

  if (req.method == "HEAD" && contentHeaders) {
    // Carry over Content-Length header on HEAD requests.
    let len = contentHeaders.get("Content-Length");
    if (len) {
      headers["Content-Length"] = len;
    }
  }

  return new Response(contentBody, {headers, status});
}

let TYPES = {
  txt: "text/plain;charset=utf-8",
  html: "text/html;charset=utf-8",
  htm: "text/html;charset=utf-8",
  css: "text/css;charset=utf-8",
  js: "text/javascript;charset=utf-8",
  md: "text/markdown;charset=utf-8",
  sh: "application/x-shellscript;charset=utf-8",
  svg: "image/svg+xml;charset=utf-8",
  xml: "text/xml;charset=utf-8",

  png: "image/png",
  jpeg: "image/jpeg",
  jpg: "image/jpeg",
  jpe: "image/jpeg",
  gif: "image/gif",

  ttf: "font/ttf",
  woff: "font/woff",
  woff2: "font/woff2",
  eot: "application/vnd.ms-fontobject",

  // When serving files with the .gz extension, we do NOT want to use `Content-Encoding: gzip`,
  // because this will cause the user agent to unzip it, which is usually not what the user wants
  // when downloading a gzipped archive.
  gz: "application/gzip",
  bz: "application/x-bzip",
  bz2: "application/x-bzip2",
  xz: "application/x-xz",
  zst: "application/zst",
}

async function makeListingHtml(path, listing, dir) {
  if (!path.endsWith("/")) path += "/";

  let htmlList = [];
  for (let file of listing) {
    let len, modified;
    if (file.type == "file" || file.type == "directory") {
      let meta = await dir.fetch("http://dummy" + path + file.name, {method: "HEAD"});
      console.log(meta.status, "http://dummy" + path + file.name, meta.headers.get("Content-Length"));
      len = meta.headers.get("Content-Length");
      modified = meta.headers.get("Last-Modified");
    }

    len = len || `(${file.type})`;
    modified = modified || "";

    htmlList.push(
        `      <tr>` +
        `<td><a href="${encodeURIComponent(file.name)}">${file.name}</a></td>` +
        `<td>${modified}</td><td>${len}</td></tr>`);
  }

  return `<!DOCTYPE html>
<html>
  <head>
    <title>Index of ${path}</title>
    <style type="text/css">
      td { padding-right: 16px; text-align: right; }
      td:nth-of-type(1) { font-family: monospace; text-align: left; }
      th { text-align: left; }
    </style>
  </head>
  <body>
    <h1>Index of ${path}</h1>
    <table>
      <tr><th>Filename</th><th>Modified</th><th>Size</th></tr>
      ${htmlList.join("\n")}
  </body>
</html>
`
}
