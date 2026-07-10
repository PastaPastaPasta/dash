# BLAKE3 (vendored)

Portable C implementation of BLAKE3, vendored from
https://github.com/BLAKE3-team/BLAKE3 at tag `1.8.5` (`c/` directory).

Only the portable backend is vendored (`blake3.c`, `blake3_dispatch.c`,
`blake3_portable.c`); it is compiled with the `BLAKE3_NO_SSE2`,
`BLAKE3_NO_SSE41`, `BLAKE3_NO_AVX2`, `BLAKE3_NO_AVX512` and (implicitly, by not
defining `BLAKE3_USE_NEON`) no-NEON configuration, so no SIMD sources are
required. BLAKE3 is used exclusively by the Dash Platform GUI client library
(`--enable-platform-gui`) for GroveDB merk proof verification; it is not linked
into `dashd` or any consensus code.

Do not modify these files. To update, re-copy from a tagged upstream release
and update this README and the build definitions in `src/Makefile.am`.

License: Apache-2.0 OR CC0-1.0 (see `LICENSE`).
