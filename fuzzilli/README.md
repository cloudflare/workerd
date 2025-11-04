# Fuzz workerd with Fuzzilli
This folder contains the capnp configuration, JavaScript mocks for certain APIs which communicates with a Fuzzilli.


## Capnp config and JavaScript
The base configuration can be found in `worker.js` and `config.capnp`.
The folder contains a `config-full.capnp`` and `worker-full.js` file which imports the to-date fuzzed APIs.
To test a certain API import it in the used `.js` file. 

## REPRL

The main execution looks as follows:
- Fuzzilli starts workerd.
- Workerd will read in the capnp configuration, pointing to a script that imports the custom API endpoints, e.g. (workerd config, worker.js).
- Workerd will import the dependencies and call Stdin.reprl
- Workerd then opens the pipes and a shared memory sending a HELO to Fuzzilli, which responds with a HELO back.
- Fuzzilli sends the exec command, the length of the script to execute and the script itself.
- Workerd will then compile and execute the script similar to the eval command in a separate scope


## Test if REPRL works

```bash
bazel test --config=fuzzilli //src/workerd/tests:test-reprl-kj --action_env=CC=/usr/bin/clang-19 --test_timeout=5 --test_output=all
```

From the Fuzzilli directory test if the REPRL interface works by running:

```bash
# in Fuzzilli folder
swift run REPRLRun <path-to-workerd> fuzzilli <path-to-capnp-config> --experimental
```

## Fuzz with corpus

```bash
swift run -c release FuzzilliCli --inspect=all --profile=workerd <path-to-workerd-root>/bazel-bin/src/workerd/server/workerd --additionalArguments=<path-to-workerd-root>/samples/reprl/config-full.capnp,--experimental --storagePath=fs-new --jobs=30 --importCorpus=<path-to-corpus> --corpusImportMode=full --staticCorpus
```
