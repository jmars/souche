#!/usr/bin/env python3
"""tests/check_transcript.py -- gate the oracle transcript against tests/model.py.

Every leaf that tests/model.py produces must be present in the transcript with
the same value. Extra transcript fields (segments, control registers, x87, the
image digest, the limit) are not modelled here and are only checked to exist.

usage: check_transcript.py <expected.json> <transcript.json>
"""

import json
import sys


def walk(expected, actual, path, failures):
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            failures.append("%s: expected an object, got %r" % (path, type(actual).__name__))
            return
        for key in sorted(expected):
            if key not in actual:
                failures.append("%s.%s: missing from transcript" % (path, key))
                continue
            walk(expected[key], actual[key], "%s.%s" % (path, key), failures)
        return

    if expected != actual:
        failures.append("%s: model=%r transcript=%r" % (path, expected, actual))


def main():
    with open(sys.argv[1]) as handle:
        expected = json.load(handle)
    with open(sys.argv[2]) as handle:
        transcript = json.load(handle)

    failures = []
    walk(expected, transcript, "", failures)

    if failures:
        for failure in failures:
            print("FAIL " + failure, file=sys.stderr)
        print("check_transcript: %d mismatch(es)" % len(failures), file=sys.stderr)
        sys.exit(1)

    print("check_transcript: transcript matches tests/model.py "
          "(8 GPRs, EFLAGS, EIP, %d instructions, %d-byte window sha256)"
          % (expected["instruction_count"], expected["memory_window"]["bytes"]))


if __name__ == "__main__":
    main()
