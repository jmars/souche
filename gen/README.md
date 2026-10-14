# gen/ -- the C code generator (placeholder)

M0 adds nothing here.

Later units read the reference implementation's instruction tables
(`v86/gen/x86_table.js`, plus `v86/src/rust/gen/`) and emit the C tables and
the decoded-instruction dispatch the port's interpreter is built from. The
generator runs at build time under node; nothing in it is downloaded from the
network.
