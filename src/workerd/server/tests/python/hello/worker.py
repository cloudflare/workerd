def test():
    from js.WebAssembly import Suspending

    Suspending  # noqa: B018

    # This just tests that nothing raises when we run this. It isn't great though
    # because we don't test whether we printed anything.
    # TODO: update this to test that something happened
    print("Does this work?")
