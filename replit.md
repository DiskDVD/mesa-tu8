# Mesa 3D Graphics Library

## Overview

This is the [Mesa 3D Graphics Library](https://mesa3d.org) — an open-source implementation of OpenGL, Vulkan, and other graphics APIs. It is a large C/C++ systems-level project (not a web application).

## Project Structure

- `src/` — C/C++ source code for all Mesa drivers and components
- `docs/` — Sphinx-based documentation (RST format)
- `docs-html/` — Pre-built HTML documentation (served as the web preview)
- `include/` — Public header files
- `subprojects/` — Meson subproject dependencies
- `bin/` — Build helper scripts
- `meson.build` / `meson.options` — Meson build system configuration
- `serve_docs.py` — Simple Python HTTP server for the docs

## Build System

Mesa uses **Meson** with **Ninja** for building. The C/C++ library itself requires significant system dependencies (LLVM, DRM, etc.) that are beyond what Replit's environment provides for a full build.

## Web Preview

The web preview serves the pre-built Mesa 3D documentation (built from `docs/` using Sphinx). It runs on port 5000 via `serve_docs.py`.

To rebuild the docs manually:
```sh
cd docs && sphinx-build -b html -D extensions="bootstrap,depfile,formatting,nir,redirects" . ../docs-html
```

## Deployment

Configured as a static site deployment serving `docs-html/`.
