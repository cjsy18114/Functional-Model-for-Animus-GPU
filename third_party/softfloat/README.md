# SoftFloat 3e integration

This directory vendors the unmodified upstream C/header subset listed in
`sources.txt`. See `UPSTREAM.txt` for source URL/archive SHA256 and `COPYING.txt`
for the upstream BSD license. No download is needed when building the model.

`platform.h` is project glue for a little-endian host with C11 and 64-bit integer
support. Build these sources as C with `SOFTFLOAT_FAST_INT64`; the C++ adapter
includes `softfloat.h` with C linkage. The manifest includes helper lookup tables.
Unused upstream formats/functions are excluded, while common headers retain
upstream declarations.

`src/fp_backend.cpp` adapts the 8086-SSE specialization to RISC-V: canonical
arithmetic NaN, RISC-V invalid conversion clipping, after-rounding tininess,
and all five rounding modes. Guest fcsr is stored separately for every lane in
the explicitly selected experimental policy. Backend globals are saved/restored
per operation; concurrent host callers require external synchronization or a TLS
build. Guest warps are scheduled on one host thread.

The backend's f64-to-f32 conversion is also used to quantize SFU host-double
references. This does not add any guest double-precision instructions. SFU
accuracy remains a separate, unapproved profile rather than a SoftFloat promise.
