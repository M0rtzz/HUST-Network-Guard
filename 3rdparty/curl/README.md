# Bundled libcurl for Windows

This directory contains the files required to build and run HUST-Network-Guard with a
64-bit MinGW-w64 compiler on Windows.

- Package: curl 8.21.0_3 for x64 MinGW
- Upstream: https://curl.se/windows/
- Archive: `curl-8.21.0_3-win64-mingw.zip`
- SHA-256: `773920297b25a38193f9f28a496981eceee71d6ec658e5607227681be575f976`
- License: curl license in `COPYING.txt`; bundled dependency notices are in
  `licenses/`

The bundled files are limited to the public headers, the DLL import library,
and the runtime DLL. The runtime DLL contains its third-party protocol and
compression dependencies; its remaining dependencies are Windows system DLLs.
`BUILD-MANIFEST.txt` and `BUILD-HASHES.txt` are preserved from the official
package for provenance and file verification.

Keep the headers, import library, and runtime DLL on the same version when
updating this dependency.
