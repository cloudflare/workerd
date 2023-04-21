# Using workerd with devtools

To use workerd with devtools, you need to undertake the following steps:

1. Add a `sourceURL` comment to your JavaScript sources pointing to the source files on the local machine. For example, if your workerd sources are under your home directory in `/home/joe/code/workerd/` and you are working on `samples/helloworld`, then add a `sourceURL` to `samples/helloworld/worker.js` that is a file URL to itself:

   `//# sourceURL=file:///home/joe/code/workerd/samples/helloworld/worker.js`

2. Launch `workerd` with the devtools inspector enabled. For example, to enable the inspector for localhost:

   `bazel-bin/src/workerd/server/workerd -ilocalhost samples/helloworld/config.capnp`

Support for automatically adding `sourceURL` and `sourceMapURL` for [Wrangler](https://developers.cloudflare.com/workers/wrangler/) based projects is coming soon.

## Attaching Chrome devtools

In a Chrome instance on your local machine, enter `chrome://inspect` into the Chrome address bar. In the page that opens you will see `workerd: worker main` entry with a link entitled `inspect`. Click `inspect` and a `DevTools` will open, in the FileSystem tab
add a workspace folder that contains your source files (in our example above `/home/joe/code/workerd/samples/helloworld`).

In the workspace, you can open your source files, edit the code, and set breakpoints. If you set a breakpoint in your script and then run your script by opening a URL that points to the entrypoint (for `helloworld`, this is http://localhost:8080). When execution of your script hits a breakpoint, the debugger will be presented to you.

For more details on using devtools for debugging, see https://developer.chrome.com/docs/devtools/javascript/.

## Attaching Cloudflare devtools

The Cloudflare devtools are a customized version of the Chrome devtools for developers using Cloudflare. They are part of the [Cloudflare Workers SDK](https://github.com/cloudflare/workers-sdk) along with Wrangler. Because Cloudflare devtools are aware
of workers, it requires fewer clicks to get started.

To use the Cloudflare devtools, start `workerd` locally with your workerd configuration, then open https://devtools.devprod.cloudflare.dev/js_app?ws=localhost:9229/main on your machine. This will bring up a devtools window with the source code associated with your workerd configuration. Just like the Chrome devtools, you can edit and save code, and set breakpoints.

## Debugging the devtools issues

Support for devtools is experimental. If you encounter an issue, please enable the protocol inspector in devtools and file a bug report describing the issue with the protocol log attached. See https://chromedevtools.github.io/devtools-protocol/#:~:text=Listening%20to%20the%20protocol for details on enabling the protocol inspector. Thanks!
