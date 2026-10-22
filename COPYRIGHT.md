# Copyright and Credits

This file records the copyright holders, authorship, and third-party credits
for Nodosumi. It is kept in the repository to satisfy attribution and license
requirements.

## License

Nodosumi is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE.txt](LICENSE.txt) for the full license text.

## Copyright holders

- **Nodosumi core, modifications, and branding**
  Copyright (C) 2026 the Nodosumi contributors.
- **Emulation core lineage**
  Copyright (C) Anthony Pesch and the Redream contributors.

Nodosumi's emulation core incorporates GPLv3-licensed code from the Redream
Dreamcast emulator project. The core has been substantially modified, extended,
and reorganized for Nodosumi; those modifications are copyright (C) 2026 the
Nodosumi contributors and are released under the same GPLv3-or-later terms.

See the project history for the full list of Nodosumi contributors.

## Techniques and behavioral references

Emulation techniques, hardware behavior, and rendering approaches used in
Nodosumi were also informed by the Redream and Flycast emulator projects. No
Flycast source code is included here; these are independent implementations of
the same documented and observed Dreamcast behavior.

## Third-party libraries

Bundled dependencies under `deps/` are distributed under their own licenses.
See the `LICENSE`, `COPYING`, or `LICENSE.txt` file inside each dependency
directory for details. These include, among others:

- SDL2 (zlib license)
- Capstone (BSD 3-clause)
- libchdr and its bundled codecs (BSD and per-codec terms)
- VIXL (BSD 3-clause)
- Dear ImGui (MIT)
- glad (MIT / public domain)
- inih (BSD 3-clause)

## Bundled fonts

The UI assets under `assets/` embed the following fonts:

- **Font Awesome 4** (Copyright Dave Gandy) - SIL Open Font License 1.1
  (`assets/LICENSE-OFL-1.1.txt`)
- **Open Sans** (Copyright Google / Ascender Corporation) - Apache License 2.0
  (`assets/LICENSE-APACHE-2.0.txt`)
- **Oswald** (Copyright The Oswald Project Authors) - SIL Open Font License 1.1
  (`assets/LICENSE-OFL-1.1.txt`)

## Trademarks

"SEGA" and "Dreamcast" are trademarks or registered trademarks of SEGA
Corporation. Nodosumi is not affiliated with, endorsed by, or sponsored by
SEGA. No BIOS, ROM, or game data is included with or distributed by this
project.
