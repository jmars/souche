#!/usr/bin/env python3
"""tools/m1c-mutate.py -- apply ONE exact-string mutation, refusing anything ambiguous.

Used by results/m1c-mut-rig.sh.  The old text comes from MUT_OLD and the new from
MUT_NEW; a target that does not appear EXACTLY once is a refusal, not a partial edit --
an ambiguous mutation is how a rig edits the wrong site and reports a false asymmetry.
"""
import os
import sys

path = sys.argv[1]
old = os.environ["MUT_OLD"]
new = os.environ["MUT_NEW"]
text = open(path).read()
count = text.count(old)
if count != 1:
    sys.exit("mutation target not unique in %s: %d occurrences" % (path, count))
open(path, "w").write(text.replace(old, new))
