# Third-party components

Conduit's own code is MIT-licensed (see `LICENSE`). The built `default.xbe` additionally contains the
following components, linked in through [nxdk](https://github.com/XboxDev/nxdk). If you distribute a built
XBE or ISO, include this file; the full license texts are in the nxdk tree (`nxdk/LICENSES`, `nxdk/lib/**`).

| Component | License | Used for |
|---|---|---|
| nxdk (incl. pbkit, runtime library) | MIT (parts: see `nxdk/LICENSES`) | toolchain, kernel/hardware layer, network driver |
| lwIP | BSD 3-Clause | TCP/IP stack and socket API |
| SDL2 | zlib | window, renderer, game controller input |
| SDL_ttf | zlib | text rendering |
| FreeType 2.4.12 | FreeType License (or GPLv2) | glyph rasterizer for SDL_ttf (used under the FreeType License) |
| zlib | zlib | unpacking the embedded artwork |
| PDCLib | CC0 | C library |

## Fonts (embedded in the binary)

Both are under the SIL Open Font License 1.1, unmodified; license texts are in `assets/fonts/`.

- **Michroma** - Copyright 2011 The Michroma Project Authors
- **Saira Semi Condensed** - Copyright 2016 The Saira Project Authors, reserved font name "Saira"

## Trademarks

"Xbox" and the Xbox Dashboard are trademarks / works of Microsoft Corporation. Conduit is an independent
hobby project, not affiliated with or endorsed by Microsoft. It contains **no** Microsoft code, textures,
fonts or other assets; the look is an original imitation of the dashboard's style.

At runtime, Conduit can *read* one texture (the tunnel's cell pattern) from the dashboard installed on the
user's own console (`C:\xboxdashdata.*\default.xip`). That file is neither included in this repository nor
copied, saved or redistributed; it is used in memory only. Without it, a built-in original pattern is used.
