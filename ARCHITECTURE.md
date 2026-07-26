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

---

## Appendix — Interne interfaces v0.1 (architect, 2026-07-26)

Contract: `src/core_internal.h` (volledige kloktopologie + bronnen staan daar in de kop).
Tijdelijke linkbaarheid: `src/stubs_tmp.c` — per functie gemarkeerd `TIJDELIJK: vervangen
door <bestand>`; elke bouwer verwijdert bij oplevering alléén zijn eigen blok, de
integrator gooit het bestand weg zodra alle vier de echte implementaties er zijn.

### Gemaakte keuzes

1. **Kloktopologie (onderzocht + vastgelegd):** NTSC XTAL 7.15909 MHz, CPU-pinklok
   ×3/4, machinecyclus = XTAL/20; PAL XTAL 17.734475 MHz, CPU-pinklok /3,
   machinecyclus = XTAL/45 (PAL-CPU is ~10% sneller). VDC-klok XTAL/2 (NTSC) resp.
   /5 (PAL), pixelklok = 2× VDC-klok, htotal 455/456 px. Frames: NTSC 263 lijnen,
   PAL 313; actief beeld in beide regio's lijn 0..241, VBLANK vanaf 242. Bronnen:
   MCS-48-datasheet (÷15) + MAME `odyssey2.cpp`/`i8244.cpp` (gedragsfeiten, geen code).
2. **Scheduler exact rationaal:** CPU-cycles per scanline zijn exact 91/4 (NTSC)
   en 76/3 (PAL). `g7k_run_frame` gebruikt een breuk-accumulator + overshoot-schuld
   (2-cycle-instructies over de lijngrens) → nul drift over een frame; geverifieerd:
   4 NTSC-frames = exact 23933 machinecycli.
3. **Structs volledig in `core_internal.h`,** alle subsysteem-state embedded in
   `g7k_sys` (één calloc in `g7k_create`, cart-ROM als vast 16K-array). Nodig omdat
   `state.c` alle structs serialiseert. Regel: bouwers wijzigen alleen hun eigen
   struct-blok; signaturen wijzigen = overleg + versie-impact.
4. **CPU is klok-agnostisch:** kent alleen "cycles per instructie" (C6); sys bepaalt
   het lijnbudget. Alle omgevingstoegang via `cpu8048_bus`-callbacks (ROM-fetch,
   MOVX, P1/P2/BUS, T0/T1); `cpu8048_reset` pompt P1=P2=0xFF door de callbacks →
   cart boot uit bank 3 (C8/M3) zonder speciale gevallen.
5. **VDC-lockstep-paar** `begin_line`/`render_line`: status/T1 kloppen tijdens de
   CPU-burst van een lijn, renderen (pixels+collision+audio-tick) gebeurt erna →
   mid-line registerwrites zichtbaar (V1). T1 = VBL **of** HBL (C7, MAME-feit);
   HBLANK-fase via `vdc8244_hblank_at(cycles_into_line)`.
6. **Bus-decodering in sys.c** (gedragsfeiten MAME): VDC-read bij `(P1&0x48)==0`,
   VDC-write bij P13 laag; ext-RAM bij P14 laag én A7 laag (128B, M5); open bus
   0xFF, overlap AND-t. Keyboard: P12 laag activeert scan, rij = P2[0:2] (74156),
   antwoord via 74148 op P2[7:4]; joysticks op BUS bij rijselect 0/1 —
   bitvolgorde/spelertoewijzing definitief te verifiëren in de S4-test.
7. **Reset-semantiek (S1):** warm = alleen 8048-RESET-pin (RAM/VDC blijven staan);
   koud = ook iram/extram/VDC/fb/cycli gewist. De VDC heeft geen reset-lijn.
8. **Audio:** VDC pusht 2 samples per scanline in een ringbuffer; samplerate =
   2× lijnfrequentie → 31469 Hz (NTSC) / 31113 Hz (PAL). Ring wordt op framegrens
   geleegd zodat `g7k_audio_read` exact "het laatste frame" levert.
9. **Framebuffer v0.1:** 320×240 (160 VDC-pixels horizontaal verdubbeld; 240 van
   242 actieve lijnen). Runtime-API, dus later vrij aanpasbaar.
10. **Input-latching op framegrens** (pending→live) als fundament voor
    deterministische record/replay (QUIRKS-ontwerpprincipe 4).
11. **Open punten:** `g7k_key_from_char` levert nog `G7K_KEY_NONE` — de
    teken→matrixcode-tabel hoort bij de S4-bouwer (geen goktabel in het contract);
    T0 ongebruikt tot The Voice-fase; PAL-lijnfrequentie bewust de interne
    8245-telling (15556.6 Hz, slave-mode-kanttekening in `core_internal.h`).
