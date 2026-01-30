Quick notes for Python bindings

- The extension module is built into `python/_regjit.so` via the Makefile target `python-bindings`.
- API:
  - `_regjit.Regex(pattern)` - compile pattern on construction; call `.match(s)` or `.match_bytes(b)`
  - The module uses the C API in `src/regjit_capi.h` and will compile patterns into the in-process JIT.

Build:

  make libregjit.so
  make python-bindings

Usage:

  python3.12 -c "import sys; sys.path.insert(0, 'python'); from _regjit import Regex; r=Regex('ab*c'); print(r.match('abbc'))"

Tests:

  # run the basic test
  python3.12 python/tests/test_bindings.py
