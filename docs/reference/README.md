# Reference Documents

`WebMite_User_Manual.pdf` (MMBasic 5.08.00, Revision 3) is the last standalone
WebMite manual before the WiFi documentation was folded into the PicoMite
manual; sourced from geoffg.net's archive. Note it post-dates the removal of
the 5.07-era TLS client commands (`WEB OPEN TLS CLIENT` / `WEB TLS CLIENT
REQUEST` / `WEB CLOSE TLS CLIENT`), so those appear in neither manual — the
community "MMBASIC WEB ADDENDUM" (pwillard.com) documents them. The ESP32
port implements that historical TLS syntax.

`PicoMite_User_Manual.pdf` is kept here as the canonical in-repository copy.
During the Stage 2 root-directory cleanup, the root copy was compared against
this file and removed because it was older and smaller:

- root `PicoMite_User_Manual.pdf`: SHA-256 `3da427a9370a3208159c058bf634c3feaebf42806fbc70bcc6eaa68763fbb1d6`, 225 pages, created 2024-12-29
- `docs/reference/PicoMite_User_Manual.pdf`: SHA-256 `a60e75cc62cbc8d1384e8779ef8fccae41d02bad41800c7acfb7f1c1ed61f91a`, 253 pages, created 2026-03-25
