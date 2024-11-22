## How to add WPT test suites to workerd

[WPT groups its test suites into directories](https://github.com/web-platform-tests/wpt). To add a new test suite to workerd, simply create a file named `suite-test-config.js`. For example, `url/` tests are added in the `url-test-config.js` file.

A test config file is expected to export an object named `config`. This object can be empty to run all tests and subtests.

```js
export const config = {};
```

A test export is generated for each JS file in the directory. WPT test files are sometimes located in subdirectories, but the test importer flattens the hierarchy within the test suite.

Each test file can contain several subtests, created by invoking `test` or `promise_test` within the test file. They are named according to the message argument provided.

## Options

Each entry in the `config` object specifies the test file name, and the options to apply to those tests. For example, `urlsearchparams-sort.any.js` can be configured using:

```js
export const config = {
    'urlsearchparams-sort.any.js': {
        expectedFailures: ['Parse and sort: ﬃ&🌈', 'URL parse and sort: ﬃ&🌈'],
    },
};
```


The following options are currently supported:

* `ignore: bool`: Don't import a test file that is irrelevant for workerd tests
* `skippedTests: string[]`: A list of subtests that should not be executed. This should only be used for subtests that would crash workerd.
* `expectedFailures: string[]`: A list of subtests that are expected to fail, either due to a bug in workerd or an intentional choice not to support a feature.

## Implementation

Once a test config file is detected, the `wpt_test` macro is invoked for the suite. A JS test file is created that invokes the test harness on each JS file within the suite.

A WD test file is generated which links all of the necessary components:

* Generated JS test file (e.g. `url-test.js`)
* The test config file (e.g. `url-test-config.js`)
* Test harness (`harness.js`)
* Test files provided by WPT  (e.g. `url/urlsearchparams-sort.any.js`)
* JSON resources provided by WPT (e.g. `url/resources/urltestdata.json`)

