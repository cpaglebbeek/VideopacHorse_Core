# VideopacHorse_Core

Portable **C11 emulator-core** voor de Philips Videopac G7000 / Magnavox Odyssey² —
Intel 8048 CPU + Intel 8244/8245 VDC (video, audio én hardware-collision-detection),
cycle-getrouw en zonder per-game hacks.

Eén publieke API (`include/g7000.h`), drie frontends in zusterrepos:

| Frontend | Repo | Route |
|---|---|---|
| Web (WASM) | [VideopacHorse_Web](https://github.com/cpaglebbeek/VideopacHorse_Web) | Emscripten |
| Android | [VideopacHorse_Android](https://github.com/cpaglebbeek/VideopacHorse_Android) | NDK/JNI |
| Steam Deck | [VideopacHorse_SteamDeck](https://github.com/cpaglebbeek/VideopacHorse_SteamDeck) | SDL2/Flatpak |

Regie: [Meta_VideopacHorse](https://github.com/cpaglebbeek/Meta_VideopacHorse).

## Bouwen

```bash
make          # libg7000.a
make test     # testsuite (ASan/UBSan) — BIOS-loos zelfvoorzienend
make wasm     # build/wasm/g7000.{js,wasm} (vereist emscripten)
```

## ROMs & BIOS

Deze repo bevat **geen** BIOS- of game-ROMs (auteursrechtelijk beschermd) en levert ze
nooit mee. De emulator laadt ze runtime via de API (`g7k_load_bios`, 1024 bytes;
`g7k_load_cart`, raw dump). Optionele echte-ROM-tests lezen paden uit `G7K_BIOS`/`G7K_ROMDIR`
en skippen stil als die ontbreken.

## Herkomst & licentie

AGPL-3.0, 100% from-scratch. Gedragskennis komt uit datasheets (Intel MCS-48, 8244/8245),
publieke feiten (MAME-gedrag, No-Intro hashes) en de analyse in `docs/O2EM_DEEPDIVE.md`;
er is geen code overgenomen uit O2EM (Clarified Artistic License) of andere emulators.
`docs/QUIRKS.md` is de volledige hardware-gedragscanon met teststatus.
