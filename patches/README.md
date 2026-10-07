SIMDe remains pinned to `71fd833d9666141edcd1d3c109a80e228303d8d7`.
The native `__m64` streaming-store compatibility fix is recorded in
`simde-native-m64-stream.patch`; no private vendor commit is required.

After initializing the SDK submodules, run from the SDK root:

```sh
python3 scripts/prepare_simde.py
python3 scripts/prepare_simde.py --check
```

The preparer accepts the exact repository root and revision, preserves unrelated
local edits, and verifies the actual patched source independently of Git's exit
status. A partial/conflicting patch fails rather than overwriting local edits.
This step is required before building with Windows Clang toolchains that
represent native `__m64` as a union rejected by `__builtin_nontemporal_store`.
The public SIMD type and its eight stored bytes remain unchanged.

Offline preparation regression, using disposable clones of locally available
Git objects (no network or original-checkout writes):

```sh
python3 tests/regression/test_prepare_simde.py /tmp/rex-simde-preparation
```

The separate `test_simde_stream_pi.py` fixture compiles the actual header and
checks bit patterns and adjacent canaries. Its optional `--windows-sysroot`
mode also checks the real Windows Clang non-temporal `i64` store. Preparation
checks do not substitute for compiler or runtime validation.
