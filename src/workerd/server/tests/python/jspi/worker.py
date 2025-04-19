from asyncio import sleep

from pyodide import __version__


def jspi_sleep():
    if __version__ == "0.26.0a2":
        print("JSPI not supported on 0.26.0a2, skipping")
        return

    from pyodide.ffi import run_sync

    run_sync(sleep(0.1))
    print("Okay")


print("Testing JSPI at top level...")
jspi_sleep()


def test():
    print("Testing JSPI in handler...")
    jspi_sleep()
