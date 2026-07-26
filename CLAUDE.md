# CLAUDE.md — VideopacHorse_Core

Portable C11 emulator-core voor de Philips Videopac G7000 (Magnavox Odyssey²).
Onderdeel van het **Gaming-ecosysteem**, VideopacHorse-familie (5 repos, lock-step versies):
`Meta_VideopacHorse` (regie) · `VideopacHorse_Core` (dit, de engine) · `_Web` · `_Android` · `_SteamDeck`.

## Kernregels

1. **Publieke API = `include/g7000.h`** — frontends gebruiken uitsluitend deze header. Wijzigingen aan de API zijn ALTIJD "Oranje" (+0.1.0) of hoger en vereisen sync met alle drie de port-repos.
2. **Geen platform-code in de core.** Geen I/O, geen threads, geen globals, malloc alleen in create/destroy. C11, `-Wall -Wextra -Werror`.
3. **GEEN ROMs in git.** BIOS (`o2rom.bin` e.a.), game-dumps en de Manopac homebrew-tools zijn copyrighted/ongelicenseerd — alleen hashes als test-fixtures. `.gitignore` blokkeert `*.bin`/`*.rom`; NOOIT omzeilen.
4. **Licentie-brandmuur:** AGPL-3.0, 100% from scratch. Géén code/tabellen uit O2EM (Clarified Artistic License), MAME of andere emulators kopiëren — alleen gedrag/feiten/datasheets (verantwoording: `docs/O2EM_DEEPDIVE.md`). De 8244-charset opbouwen uit chip-documentatie, herkomst documenteren.
5. **Nul per-game hacks.** Zie `docs/QUIRKS.md` ontwerpprincipes. Een game die niet draait = timing/gedragsbug in de core, geen aanleiding voor een CRC-hack.
6. **Elke quirk uit `docs/QUIRKS.md` heeft een expliciete test** in `tests/`. Nieuwe hardware-kennis → eerst QUIRKS.md-regel + test, dan implementatie.
7. **Testen:** `make test` (met ASan/UBSan) moet groen zijn vóór elke commit die src/ raakt. Testsuite is BIOS-loos zelfvoorzienend (eigen minimale 8048-programma's als byte-arrays); tests met echte BIOS/ROMs skippen automatisch als de bestanden ontbreken (pad via env `G7K_BIOS`, `G7K_ROMDIR`).

## Versionering & codenamen

Thema: **Videopac/Odyssey²-pioniers** (Baer, Averett, Palmer, …). Lock-step over alle 5 repos.
Groen +0.0.1 (code-only) · Oranje +0.1.0 (design/API) · Rood +1.0.0 (redesign). Versie in `version.json` + `G7K_VERSION_STRING` in `g7000.h` vóór build/tag. Elke release: unieke versie + codenaam + buglijst (release-protocol Meta_Master).

## Bugfix-protocol

Kleurcodering Groen/Geel/Rood + verplichte RCA op drie niveaus (functioneel/technisch/architectonisch), BUGLIST.md volgens `Meta_Master/templates/BUGLIST_TEMPLATE.md`. Debug-protocol: STOP + RCA + WhatIf; nooit een nieuwe route naast een werkende bouwen.

## Referentiemateriaal

- `docs/O2EM_DEEPDIVE.md` — gedrags-/feitenbronnen + juridische kaders
- `docs/QUIRKS.md` — hardware-gedragscanon + teststatus
- Referentie-runner op de Mac: RetroArch + libretro-o2em (native arm64), nooit de Win32 o2em.exe
- Testvectoren vp_01pl/vp_14/vp_24: zie QUIRKS.md M7 (hashes in deep-dive §2)

## Sessieprotocol

Meta_Master-protocollen gelden onverkort: WhatIf vóór actie, prompt-sessies in `prompts/`, statusblok, ZSH-safety, OEU-triggers.
