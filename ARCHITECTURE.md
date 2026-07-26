# ARCHITECTURE.md — VideopacHorse_Core

## Doel

Cycle-getrouwe, platform-loze emulatie van de Philips Videopac G7000 (Magnavox Odyssey²):
Intel 8048-CPU + Intel 8244/8245-VDC (video, audio, collision) + 128B externe RAM +
cartridge-banking, achter één C11-API (`include/g7000.h`) die ongewijzigd draait in
WASM (Web), JNI (Android) en native SDL2 (Steam Deck).

## Componenten & relaties

```
 frontends (aparte repos)                 VideopacHorse_Core
┌───────────────┐  RGBA-fb / s16-audio  ┌──────────────────────────────┐
│ _Web (WASM)   │◄──────────────────────│ sys.c    publieke API,       │
│ _Android(JNI) │   joystick/keys ─────►│          bus, scheduling      │
│ _SteamDeck    │                       ├──────────┬───────────────────┤
└───────────────┘                       │ cpu8048.c│ vdc8244.c         │
                                        │ MCS-48   │ scanline-renderer │
      geen ROMs in git                  │ core     │ + collision       │
      BIOS/carts via caller             ├──────────┤ + audio (8244)    │
                                        │ cart.c   ├───────────────────┤
                                        │ banking  │ state.c savestates│
                                        └──────────┴───────────────────┘
```

| Component | Bestand(en) | Verantwoordelijkheid | Hangt af van |
|---|---|---|---|
| Publieke API / systeem | `src/sys.c` | create/destroy, bus-routing 0x000-0xFFF, frame-scheduling CPU↔VDC, input-latching, reset warm/koud | alle onderstaande |
| CPU | `src/cpu8048.c` (+ `src/cpu8048.h`) | volledige MCS-48: registers, 11-bit PC + MB, timer/counter, interrupts, T0/T1, poorten P1/P2/BUS, cycle-teller | bus-callbacks uit sys |
| VDC + audio | `src/vdc8244.c` (+ `.h`) | scanline-renderer (chars/quads/sprites/grid/dots), collision-bitmatrix, VBLANK/HBLANK-status + IRQ, toon+ruis-generator, palet | klok uit sys |
| Cartridge | `src/cart.c` (+ `.h`) | mapping-formule (QUIRKS M4), P1-live-banking, mirror 0xC00, open-bus 0xFF | P1 uit cpu |
| Savestate | `src/state.c` | versioned blob, header met BIOS-CRC32 + machinetype + regio | alle state-structs |
| Tests | `tests/*.c` | harness (`test_main.c`) + per-quirk tests (QUIRKS C/M/V/A/S-nummers in testnaam) | publieke API + interne headers |

## Data-flow per frame

`g7k_run_frame` → sys klokt CPU en VDC in lock-step per scanline (mid-scanline
registerwrites zichtbaar, QUIRKS V1) → VDC schrijft scanline naar RGBA-framebuffer en
audio-samples naar ringbuffer → VBLANK zet T1 + externe IRQ → frontend leest
framebuffer/audio en zet input voor het volgende frame.

## Oorzaak/gevolg-matrix (wijzigings-impact)

| Als dit wijzigt | Dan mee-controleren |
|---|---|
| `g7000.h` | alle 3 port-repos + Makefile-EXPORTED_FUNCTIONS (wasm) + versie Oranje |
| CPU-timing | VDC-sync (V1), audio-pitch (A1), alle canary-tests |
| VDC-registerlayout | savestate-formaat (S3), collision-tests (V3) |
| cart.c-mapping | QUIRKS M-tests + bank-CRC-vectoren (M7) |
| savestate-struct | state-versienummer bumpen + migratie-test |

## Ontwerpbeslissingen

1. **Scanline-lockstep i.p.v. frame-based** — bewezen noodzakelijk (QUIRKS V1); goedkoop genoeg voor een 1978-console, ook in WASM.
2. **RGBA8888-framebuffer + mono s16-audio als contract** — geen palet-indexen naar buiten: frontends blijven triviaal en het interne palet (O2 vs VP+ later) kan vrij evolueren.
3. **Runtime fb-afmetingen** (`g7k_fb_width/height`) — geen compile-time constants in de API, zodat overscan-keuzes de ports niet breken.
4. **BIOS-loos testbaar** — testsuite injecteert eigen 8048-programma's; echte-ROM-tests zijn optioneel en skippen zonder bestanden (CI-veilig, juridisch schoon).
5. **Fasering** — G7000-basis eerst; VP+ (EF9340/41), C7420 (Z80-in-cart) en The Voice (sample-playback) als latere, geïsoleerde modules (deep-dive §7.8).

## Relaties met andere projecten

- **Meta_VideopacHorse** — regie, lock-step-versies, ecosysteem-docs
- **VideopacHorse_Web/_Android/_SteamDeck** — consumenten van `g7000.h`
- **Meta_Master** — protocollen, PROJECTS.json, STATUS
- **SteamDeckMSX** — herbruikt deploy-route `/Deploy2SteamDeck` voor de Deck-port
