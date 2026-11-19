# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

"""Checks that `workerd repl` leaves its terminal as it found it when a signal ends it.

At the prompt, the console has the terminal in raw mode, with bracketed paste on
(src/workerd/server/cli/repl/terminal.rs). A signal that ends the REPL there restores both before
the REPL ends, as the signal ends it by default, since neither the shell nor a program that runs
the REPL on a pseudo-terminal need restore them. Each test runs the REPL on a pseudo-terminal of
its own.

Usage: terminal-test.py WORKERD_BINARY
"""

import os
import resource
import select
import signal
import subprocess
import sys
import termios
import time
import unittest

WORKERD = sys.argv.pop(1)

BRACKETED_PASTE_ON = b"\x1b[?2004h"
BRACKETED_PASTE_OFF = b"\x1b[?2004l"

# How long to wait for the REPL to answer before failing.
DEADLINE_SECONDS = 10

# The modes raw mode turns off, and their flag fields' indices in a tcgetattr() list.
RAW_MODE_FLAGS = {"ECHO": 3, "ICANON": 3, "ISIG": 3, "IEXTEN": 3, "IXON": 0, "ICRNL": 0}


def raw_mode_flags(attributes):
    """Which of the flags raw mode turns off `attributes`, from tcgetattr(), has on."""
    return {
        name: bool(attributes[i] & getattr(termios, name))
        for name, i in RAW_MODE_FLAGS.items()
    }


class Repl:
    """`workerd repl` on a new pseudo-terminal, started with `ignored` signals ignored."""

    def __init__(self, ignored=()):
        # The slave stays open throughout: when its last descriptor closes, Linux resets the
        # terminal's modes, which would hide what the REPL left them as.
        self.master, self.slave = os.openpty()
        self.started_with = termios.tcgetattr(self.slave)
        self.output = b""
        # Reads end of file once the REPL, which holds the other end open, has ended.
        self.running, running = os.pipe()

        def start():
            # Linux ignores a zero core limit for piped core dumps. A limit of one
            # suppresses those too (fs/coredump.c's recursive core-handler guard),
            # so SIGQUIT does not wait for a host core-dump service. Respect an
            # inherited hard limit of zero, which an unprivileged child cannot raise.
            _, hard_limit = resource.getrlimit(resource.RLIMIT_CORE)
            core_limit = 1 if sys.platform == "linux" and hard_limit != 0 else 0
            resource.setrlimit(resource.RLIMIT_CORE, (core_limit, core_limit))
            for number in ignored:
                signal.signal(number, signal.SIG_IGN)

        self.process = subprocess.Popen(
            [WORKERD, "repl", "--experimental"],
            stdin=self.slave,
            stdout=self.slave,
            stderr=self.slave,
            pass_fds=[running],
            env={**os.environ, "WORKERD_REPL_HISTORY": "", "NO_COLOR": "1"},
            preexec_fn=start,
            start_new_session=True,
        )
        os.close(running)

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        os.close(self.master)
        os.close(self.slave)
        os.close(self.running)

    def read_until(self, expected, after=b""):
        """Reads the REPL's output until `expected` follows the first `after` in it."""
        deadline = time.monotonic() + DEADLINE_SECONDS
        while not self.includes(expected, after):
            remaining = deadline - time.monotonic()
            ready = select.select([self.master, self.running], [], [], remaining)[0]
            if self.master in ready:
                self.output += os.read(self.master, 4096)
            elif self.running in ready:
                raise AssertionError(
                    f"the REPL ended without {expected!r}: {self.output!r}"
                )
            else:
                raise AssertionError(f"no {expected!r} in {self.output!r}")

    def includes(self, expected, after):
        start = self.output.find(after)
        return start >= 0 and expected in self.output[start + len(after) :]

    def wait_for_prompt(self):
        """Waits until the REPL prompts with the terminal in raw mode."""
        self.read_until(b"> ", after=BRACKETED_PASTE_ON)

    def type(self, line):
        os.write(self.master, line + b"\r")

    def end(self):
        """Waits for the REPL to end; its exit status, and what it wrote in all."""
        status = self.process.wait(DEADLINE_SECONDS)
        # It has ended, so all it wrote is there to read.
        while select.select([self.master], [], [], 0)[0]:
            self.output += os.read(self.master, 4096)
        return status, self.output

    def modes(self):
        return raw_mode_flags(termios.tcgetattr(self.slave))

    def modes_started_with(self):
        return raw_mode_flags(self.started_with)


class SignalAtPrompt(unittest.TestCase):
    def repl(self, **options):
        repl = Repl(**options)
        self.addCleanup(repl.close)
        return repl

    def check_restores_terminal(self, number):
        repl = self.repl()
        repl.wait_for_prompt()
        self.assertNotEqual(repl.modes(), repl.modes_started_with(), "not in raw mode")
        os.kill(repl.process.pid, number)
        status, output = repl.end()
        self.assertEqual(status, -number, output)
        self.assertEqual(repl.modes(), repl.modes_started_with(), output)
        self.assertGreater(
            output.rfind(BRACKETED_PASTE_OFF), output.rfind(BRACKETED_PASTE_ON), output
        )

    def test_interrupt(self):
        self.check_restores_terminal(signal.SIGINT)

    def test_terminate(self):
        self.check_restores_terminal(signal.SIGTERM)

    def test_hang_up(self):
        self.check_restores_terminal(signal.SIGHUP)

    def test_quit(self):
        self.check_restores_terminal(signal.SIGQUIT)

    def test_hang_up_ignored_at_start(self):
        # As `nohup` starts it.
        repl = self.repl(ignored=[signal.SIGHUP])
        repl.wait_for_prompt()
        os.kill(repl.process.pid, signal.SIGHUP)
        repl.type(b"1 + 1")
        repl.read_until(b"2\r\n", after=b"1 + 1")
        repl.type(b".exit")
        status, output = repl.end()
        self.assertEqual(status, 0, output)
        self.assertEqual(repl.modes(), repl.modes_started_with(), output)


if __name__ == "__main__":
    unittest.main()
