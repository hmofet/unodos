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
- **MicroPython** (the Python 3 interpreter inside the optional `PYRT.UNO`) -
  (c) 2013-2024 Damien P. George and contributors. MIT license, below. Only
  present when the Python runtime module is built into the image.
- **AAC decoding tables** (in unomedia's `aac_tables.h`) - the spectral
  Huffman codebooks, scalefactor codebook and scalefactor-band offset tables
  are derived from **OpenCORE aacdec** (Android Open Source Project),
  licensed **Apache License 2.0**, below. The AAC decoder CODE around those
  tables is this project's own (CC BY-NC 4.0); only the ISO constant data
  derives from OpenCORE. Derivation is scripted and documented in
  `unomedia/tools/mkaactables.py`.
- **MP3 decoding tables** (in unomedia's `mp3_tables.h`) - the ISO constant
  data (Huffman codebooks, synthesis window, scalefactor-band bounds) was
  sourced from **PDMP3** by Krister Lagerström, **public domain** - listed
  for provenance; no obligation attaches. The MP3 decoder code is this
  project's own.
- **World map data** (the Clock app's land mask) - derived from
  **Natural Earth** 110m land (naturalearthdata.com), **public domain** -
  listed for provenance; Natural Earth requests no attribution.
- **KallistiOS** - the Dreamcast port links against the KallistiOS
  OS/runtime (BSD-style KOS license). It is a build-time toolchain
  dependency, not vendored here, and is not part of the pc64 image this
  document ships on; its license travels with any Dreamcast distribution.

## Bundled fonts

- **ChiKareGo 2** (the default "Chicago" UI face) - (c) Giles Booth,
  **CC BY** (attribution - this notice provides it). A free-culture homage
  to Chicago; not Apple's Chicago, which is proprietary and not bundled.
- **DejaVu Sans / Sans Mono** (`SANS.TTF` / `MONO.TTF`) - Bitstream Vera
  license; DejaVu changes are public domain (full text below).
- **Ubuntu** (`UBUNTU.TTF`) - Ubuntu Font Licence 1.0 (full text below).

## Game data (Duum)

Duum, the Python Doom engine, loads a Doom-format IWAD that is **not part of
this project** and **not committed** to the repo. The shippable free default
is **Freedoom** (freedoom.github.io), a from-scratch Doom-compatible IWAD
under a **BSD 3-clause** license; a distribution that bundles `freedoom1.wad`
carries Freedoom's own `COPYING`/credits. id Software's shareware
`DOOM1.WAD` may be supplied by the developer for testing but is never
redistributed here. UnoDOS ships neither WAD by default.

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

Applies to BearSSL ((c) 2016 Thomas Pornin), uACPI ((c) 2022-2026
Daniil Tatianin), and MicroPython ((c) 2013-2024 Damien P. George and
contributors):

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


## Bitstream Vera / DejaVu font license

    Fonts are (c) Bitstream (see below). DejaVu changes are in public domain.
    Glyphs imported from Arev fonts are (c) Tavmjong Bah (see below)
    
    
    Bitstream Vera Fonts Copyright
    ------------------------------
    
    Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is
    a trademark of Bitstream, Inc.
    
    Permission is hereby granted, free of charge, to any person obtaining a copy
    of the fonts accompanying this license ("Fonts") and associated
    documentation files (the "Font Software"), to reproduce and distribute the
    Font Software, including without limitation the rights to use, copy, merge,
    publish, distribute, and/or sell copies of the Font Software, and to permit
    persons to whom the Font Software is furnished to do so, subject to the
    following conditions:
    
    The above copyright and trademark notices and this permission notice shall
    be included in all copies of one or more of the Font Software typefaces.
    
    The Font Software may be modified, altered, or added to, and in particular
    the designs of glyphs or characters in the Fonts may be modified and
    additional glyphs or characters may be added to the Fonts, only if the fonts
    are renamed to names not containing either the words "Bitstream" or the word
    "Vera".
    
    This License becomes null and void to the extent applicable to Fonts or Font
    Software that has been modified and is distributed under the "Bitstream
    Vera" names.
    
    The Font Software may be sold as part of a larger software package but no
    copy of one or more of the Font Software typefaces may be sold by itself.
    
    THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
    OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF COPYRIGHT, PATENT,
    TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL BITSTREAM OR THE GNOME
    FOUNDATION BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING
    ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
    THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM OTHER DEALINGS IN THE
    FONT SOFTWARE.
    
    Except as contained in this notice, the names of Gnome, the Gnome
    Foundation, and Bitstream Inc., shall not be used in advertising or
    otherwise to promote the sale, use or other dealings in this Font Software
    without prior written authorization from the Gnome Foundation or Bitstream
    Inc., respectively. For further information, contact: fonts at gnome dot
    org.
    
    Arev Fonts Copyright
    ------------------------------
    
    Copyright (c) 2006 by Tavmjong Bah. All Rights Reserved.
    
    Permission is hereby granted, free of charge, to any person obtaining
    a copy of the fonts accompanying this license ("Fonts") and
    associated documentation files (the "Font Software"), to reproduce
    and distribute the modifications to the Bitstream Vera Font Software,
    including without limitation the rights to use, copy, merge, publish,
    distribute, and/or sell copies of the Font Software, and to permit
    persons to whom the Font Software is furnished to do so, subject to
    the following conditions:
    
    The above copyright and trademark notices and this permission notice
    shall be included in all copies of one or more of the Font Software
    typefaces.
    
    The Font Software may be modified, altered, or added to, and in
    particular the designs of glyphs or characters in the Fonts may be
    modified and additional glyphs or characters may be added to the
    Fonts, only if the fonts are renamed to names not containing either
    the words "Tavmjong Bah" or the word "Arev".
    
    This License becomes null and void to the extent applicable to Fonts
    or Font Software that has been modified and is distributed under the 
    "Tavmjong Bah Arev" names.
    
    The Font Software may be sold as part of a larger software package but
    no copy of one or more of the Font Software typefaces may be sold by
    itself.
    
    THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
    OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL
    TAVMJONG BAH BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
    DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
    OTHER DEALINGS IN THE FONT SOFTWARE.
    
    Except as contained in this notice, the name of Tavmjong Bah shall not
    be used in advertising or otherwise to promote the sale, use or other
    dealings in this Font Software without prior written authorization
    from Tavmjong Bah. For further information, contact: tavmjong @ free
    . fr.
    
    TeX Gyre DJV Math
    -----------------
    Fonts are (c) Bitstream (see below). DejaVu changes are in public domain.
    
    Math extensions done by B. Jackowski, P. Strzelczyk and P. Pianowski
    (on behalf of TeX users groups) are in public domain.
    
    Letters imported from Euler Fraktur from AMSfonts are (c) American
    Mathematical Society (see below).
    Bitstream Vera Fonts Copyright
    Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera
    is a trademark of Bitstream, Inc.
    
    Permission is hereby granted, free of charge, to any person obtaining a copy
    of the fonts accompanying this license (“Fonts”) and associated
    documentation
    files (the “Font Software”), to reproduce and distribute the Font Software,
    including without limitation the rights to use, copy, merge, publish,
    distribute,
    and/or sell copies of the Font Software, and to permit persons  to whom
    the Font Software is furnished to do so, subject to the following
    conditions:
    
    The above copyright and trademark notices and this permission notice
    shall be
    included in all copies of one or more of the Font Software typefaces.
    
    The Font Software may be modified, altered, or added to, and in particular
    the designs of glyphs or characters in the Fonts may be modified and
    additional
    glyphs or characters may be added to the Fonts, only if the fonts are
    renamed
    to names not containing either the words “Bitstream” or the word “Vera”.
    
    This License becomes null and void to the extent applicable to Fonts or
    Font Software
    that has been modified and is distributed under the “Bitstream Vera”
    names.
    
    The Font Software may be sold as part of a larger software package but
    no copy
    of one or more of the Font Software typefaces may be sold by itself.
    
    THE FONT SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS
    OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF COPYRIGHT, PATENT,
    TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL BITSTREAM OR THE GNOME
    FOUNDATION
    BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING ANY GENERAL,
    SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, WHETHER IN AN
    ACTION
    OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF THE USE OR
    INABILITY TO USE
    THE FONT SOFTWARE OR FROM OTHER DEALINGS IN THE FONT SOFTWARE.
    Except as contained in this notice, the names of GNOME, the GNOME
    Foundation,
    and Bitstream Inc., shall not be used in advertising or otherwise to promote
    the sale, use or other dealings in this Font Software without prior written
    authorization from the GNOME Foundation or Bitstream Inc., respectively.
    For further information, contact: fonts at gnome dot org.
    
    AMSFonts (v. 2.2) copyright
    
    The PostScript Type 1 implementation of the AMSFonts produced by and
    previously distributed by Blue Sky Research and Y&Y, Inc. are now freely
    available for general use. This has been accomplished through the
    cooperation
    of a consortium of scientific publishers with Blue Sky Research and Y&Y.
    Members of this consortium include:
    
    Elsevier Science IBM Corporation Society for Industrial and Applied
    Mathematics (SIAM) Springer-Verlag American Mathematical Society (AMS)
    
    In order to assure the authenticity of these fonts, copyright will be
    held by
    the American Mathematical Society. This is not meant to restrict in any way
    the legitimate use of the fonts, such as (but not limited to) electronic
    distribution of documents containing these fonts, inclusion of these fonts
    into other public domain or commercial font collections or computer
    applications, use of the outline data to create derivative fonts and/or
    faces, etc. However, the AMS does require that the AMS copyright notice be
    removed from any derivative versions of the fonts which have been altered in
    any way. In addition, to ensure the fidelity of TeX documents using Computer
    Modern fonts, Professor Donald Knuth, creator of the Computer Modern faces,
    has requested that any alterations which yield different font metrics be
    given a different name.
    
    $Id$

## Ubuntu Font Licence 1.0

    -------------------------------
    UBUNTU FONT LICENCE Version 1.0
    -------------------------------
    
    PREAMBLE
    This licence allows the licensed fonts to be used, studied, modified and
    redistributed freely. The fonts, including any derivative works, can be
    bundled, embedded, and redistributed provided the terms of this licence
    are met. The fonts and derivatives, however, cannot be released under
    any other licence. The requirement for fonts to remain under this
    licence does not require any document created using the fonts or their
    derivatives to be published under this licence, as long as the primary
    purpose of the document is not to be a vehicle for the distribution of
    the fonts.
    
    DEFINITIONS
    "Font Software" refers to the set of files released by the Copyright
    Holder(s) under this licence and clearly marked as such. This may
    include source files, build scripts and documentation.
    
    "Original Version" refers to the collection of Font Software components
    as received under this licence.
    
    "Modified Version" refers to any derivative made by adding to, deleting,
    or substituting -- in part or in whole -- any of the components of the
    Original Version, by changing formats or by porting the Font Software to
    a new environment.
    
    "Copyright Holder(s)" refers to all individuals and companies who have a
    copyright ownership of the Font Software.
    
    "Substantially Changed" refers to Modified Versions which can be easily
    identified as dissimilar to the Font Software by users of the Font
    Software comparing the Original Version with the Modified Version.
    
    To "Propagate" a work means to do anything with it that, without
    permission, would make you directly or secondarily liable for
    infringement under applicable copyright law, except executing it on a
    computer or modifying a private copy. Propagation includes copying,
    distribution (with or without modification and with or without charging
    a redistribution fee), making available to the public, and in some
    countries other activities as well.
    
    PERMISSION & CONDITIONS
    This licence does not grant any rights under trademark law and all such
    rights are reserved.
    
    Permission is hereby granted, free of charge, to any person obtaining a
    copy of the Font Software, to propagate the Font Software, subject to
    the below conditions:
    
    1) Each copy of the Font Software must contain the above copyright
    notice and this licence. These can be included either as stand-alone
    text files, human-readable headers or in the appropriate machine-
    readable metadata fields within text or binary files as long as those
    fields can be easily viewed by the user.
    
    2) The font name complies with the following:
    (a) The Original Version must retain its name, unmodified.
    (b) Modified Versions which are Substantially Changed must be renamed to
    avoid use of the name of the Original Version or similar names entirely.
    (c) Modified Versions which are not Substantially Changed must be
    renamed to both (i) retain the name of the Original Version and (ii) add
    additional naming elements to distinguish the Modified Version from the
    Original Version. The name of such Modified Versions must be the name of
    the Original Version, with "derivative X" where X represents the name of
    the new work, appended to that name.
    
    3) The name(s) of the Copyright Holder(s) and any contributor to the
    Font Software shall not be used to promote, endorse or advertise any
    Modified Version, except (i) as required by this licence, (ii) to
    acknowledge the contribution(s) of the Copyright Holder(s) or (iii) with
    their explicit written permission.
    
    4) The Font Software, modified or unmodified, in part or in whole, must
    be distributed entirely under this licence, and must not be distributed
    under any other licence. The requirement for fonts to remain under this
    licence does not affect any document created using the Font Software,
    except any version of the Font Software extracted from a document
    created using the Font Software may only be distributed under this
    licence.
    
    TERMINATION
    This licence becomes null and void if any of the above conditions are
    not met.
    
    DISCLAIMER
    THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
    COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
    COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
    DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM OTHER
    DEALINGS IN THE FONT SOFTWARE.
