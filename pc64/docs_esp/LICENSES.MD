# Licensing

UnoDOS is an original work. Everything not listed below - the kernel, the
shell, every port, the unoui/uno3d/unosound/unomedia libraries and all of
their decoders - was written from scratch for this project and is licensed
**Creative Commons Attribution-NonCommercial 4.0** (CC BY-NC 4.0).

A small number of components ship with their own open licenses. This page is
the required notice for each; it is reachable from **System > Licenses** and
lives at `DOCS\LICENSES.MD` on the system volume.

## Bundled third-party components

- **BearSSL** (the TLS stack) - (c) 2016 Thomas Pornin. MIT license, below.
- **uACPI** (the ACPI interpreter core inside unoacpi) - (c) 2022-2026
  Daniil Tatianin. MIT license, below.
- **stb_truetype** (the TrueType rasteriser inside pc64_font) - Sean Barrett,
  public domain / MIT at your option.
- **AAC decoding tables** (in unomedia's `aac_tables.h`) - the spectral
  Huffman codebooks, scalefactor codebook and scalefactor-band offset tables
  are derived from **OpenCORE aacdec** (Android Open Source Project),
  licensed **Apache License 2.0**, below. The AAC decoder CODE around those
  tables is this project's own (CC BY-NC 4.0); only the ISO constant data
  derives from OpenCORE. Derivation is scripted and documented in
  `unomedia/tools/mkaactables.py`.

## Bundled fonts

- **ChiKareGo 2** (the default "Chicago" UI face) - (c) Giles Booth,
  **CC BY** (attribution). A free-culture homage to Chicago; not Apple's
  Chicago, which is proprietary and not bundled.
- **DejaVu Sans / Sans Mono** (`SANS.TTF` / `MONO.TTF`) - Bitstream Vera /
  DejaVu license (permissive).
- **Ubuntu** (`UBUNTU.TTF`) - Ubuntu Font Licence 1.0.

## Bundled demo content

The demo music (`MEDIA\`) and demo pictures (`PICTURES\`) were generated
procedurally by this project's own tools and are dedicated **CC0** (public
domain) - no attribution required, no third-party rights involved.

## MPEG audio formats

The MP3 patents expired worldwide in 2017; the AAC-LC patent claims relevant
to decoding lapsed around 2017-2018 (which is why major distributions ship
AAC decoders freely). UnoDOS decodes both; it encodes neither.

---

## The MIT License

Applies to BearSSL ((c) 2016 Thomas Pornin) and uACPI ((c) 2022-2026
Daniil Tatianin):

    Permission is hereby granted, free of charge, to any person obtaining
    a copy of this software and associated documentation files (the
    "Software"), to deal in the Software without restriction, including
    without limitation the rights to use, copy, modify, merge, publish,
    distribute, sublicense, and/or sell copies of the Software, and to
    permit persons to whom the Software is furnished to do so, subject to
    the following conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
    BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
    ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## Apache License 2.0 (OpenCORE aacdec tables)

Copyright (C) 1998-2009 PacketVideo

Licensed under the Apache License, Version 2.0 (the "License"); you may not
use the covered files except in compliance with the License. You may obtain
a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
License for the specific language governing permissions and limitations
under the License.

The complete Apache License 2.0 text accompanies the repository at
`unomedia/LICENSE.APACHE-2.0` and at the URL above.
