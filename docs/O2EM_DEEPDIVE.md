# Deep-dive: O2EM 1.21 (o2em-1.21.zip) + drie Videopac game-ROMs

**Datum:** 2026-07-26 · **Methode:** ultracode-workflow, 5 parallelle analisten (271K tokens, 57 tool-calls)
**Bestemming:** dit document gaat mee in `VideopacHorse_Core/docs/` zodra de repo bestaat.

---

## 1. Wat de zip is

`o2em-1.21.zip` is de **binaire Windows-distributie** van **O2EM v1.21 (unreleased, 2012)** — een *onofficiële* build door "manopac" (Mark Guttenbrunner) van de open-source Odyssey²/Videopac-emulator O2EM (origineel Daniel Boris 1996/98, later Andre de la Rocha en Arlindo Oliveira; officiële lijn stopte bij 1.18 op SourceForge).

| Bestand | Wat het is |
|---|---|
| `o2em.exe` | Emulator, PE32/Win32 (draait **niet** native op macOS; alleen via Wine/CrossOver) |
| `dis48.exe` | Intel 8048-disassembler (14KB, volledige MCS-48-tabel, `-bios`-modus) |
| `alleg44.dll` | Allegro 4.4.1 runtime (MinGW32) |
| `o2emcfg.xml` | Hoofdconfig + **per-game database van 238 CRC32-entries** met quirk-opties |
| `docs/LICENSE.TXT` | **Clarified Artistic License** (OSI-goedgekeurd) |
| `docs/O2EM.TXT` + `changelog.txt` | Handleiding + 15 jaar bugfix-geschiedenis (goudmijn, zie §4) |
| `roms/hb-tools/` | 4 homebrew-testtools van Manopac (zie §5) |
| `bios/`, `voice/` | **Leeg** — BIOS-ROMs en Voice-samples bewust niet meegeleverd (copyright) |

## 2. De drie game-ROMs — hash-exact geïdentificeerd (No-Intro)

| Bestand | CRC32 | Identificatie (ZEKER, alle hashes exact) |
|---|---|---|
| `vp_01pl.bin` (8KB) | `EE3EE642` | **Race + Spin-out + Cryptogram (Europe)** — de **Videopac+ (G7400)**-heruitgave van Videopac nr. 1. "pl" = **plus**, niet PAL |
| `vp_14.bin` (2KB) | `ABE368BF` | **Gunfighter (Europe)** — Videopac nr. 14 |
| `vp_24.bin` (2KB) | `2C9D1715` | **Thunderball (USA, Europe)** — Videopac nr. 24 "Flipper Game" (zelfde dump in beide regio's) |

**Bankstructuur vp_01pl (zelf gemeten):** echte **4×2K-banking** — alle vier 2K-blokken uniek (blok-CRC's `004DEC9C / 576D1FE3 / 0EBC9913 / 13EB8DD9`). Blok 3 (offset 0x1800) heeft de hoogste entropie en begint met een 8048-JMP zoals de 2K-games → **bootbank**, consistent met MCS-48-reset (P1=0xFF → bank 3). Blokken 1-2 zijn low-entropy — vermoedelijk G7400/EF9340-achtergronddata (hypothese, niet zeker).

**Juridisch:** alle drie zijn commerciële Philips/Magnavox-dumps — **nooit in een repo committen**; alleen de hashes als test-fixtures.

## 3. Cartridge-mapping — de kernformule (bevestigd in MAME-broncode)

- CPU `0x000–0x3FF` = 1KB BIOS **in de 8048 zelf**; cart zichtbaar op `0x400–0xBFF`; `0xC00–0xFFF` **spiegelt** `0x800–0xBFF` (niet-aangesloten A10 op de cart-poort).
- Bankselect is **geen mapper**: P1-bits 0-1 zijn direct als hoge ROM-adreslijnen bedraad → `bank = P1 & 3`, live bij **elke** P1-write.
- Eén formule dekt alles (2K/4K/8K): `chip_offset = ((addr>=0xC00 ? addr-0x800 : addr-0x400) + bank*0x800) & (bit_floor(rom_size)-1)`; out-of-range read → `0xFF` (open bus).
- **Reset-gedrag kritisch:** P1 moet op `0xFF` initialiseren, anders boot vp_01pl uit de verkeerde bank en crasht.
- **Gratis unit-test-vectoren:** eerste cart-fetch op 0x400 na reset = `0x44` (vp_01pl: file[0x1800]; vp_14/24: file[0]); per-bank-CRC's hierboven als banking-regressietest.
- Overige ROM-vormen in het ecosysteem: 3K-per-bank (KTAA), 3K-program+1K-data (`-exrom`: Musician, Four in 1 Row), 12K/16K (Trans American Rally+), MegaCART (Soeren Gust).

## 4. Hardware-kennis uit handleiding + changelog — de quirk-canon voor de core

**CPU (8048):** elk van deze punten heeft in O2EM aantoonbaar games gebroken — DAA/BCD (scores: Le Tresor Englouti, Cosmic Conflict), JNI, branches blijven binnen de 256-byte pagina (Sid the Spellbinder), PC-wrap 0x7FF→0, correcte externe-interruptvector (Killer Bees), cycle-getrouwe timings. **VBLANK ligt op de T1-ingang** — software detecteert daarmee zelf PAL(50Hz)/NTSC(60Hz).

**VDC (8244/8245):**
- **Scanline-accuraat renderen is een harde eis** — mid-screen registerwrites zonder interrupts (Power Lords, Super Cobra); frame-based renderen is bewezen onvoldoende.
- Echt ijzer **blokkeert VDC-writes zolang foreground aanstaat** — O2EM maakte hier een hack-optie van; er bestaat zelfs homebrew (Puzzle Piece Panic "emulator version") die alleen op de emulator-afwijking draait: `allowvdcalways`-games zijn ongeldige testcases.
- **Collision detection was O2EM's meest regressie-gevoelige subsysteem** (drie releases op rij kapot/gefixt). Canary-games: Killer Bees, Demon Attack, Cosmic Conflict.
- Sprite/char-details met eigen tests: double-size sprites schuiven **per 2 pixels**; quad-chars eigen tekenpad (KTAA, Black Hole/Red Baron-scores); prioriteitsvolgorde (Demon Attack, Turtles, Atlantis); geen wrap aan schermranden; shifted/smooth-modus = pseudo-hi-res (Q*bert); 64-tekens charset (zit als maskdata in de chip); dot-grid incl. rechterkolom; gridlijn-hoogtes.
- **P17 laag = alle VDC-kleuren fel** (Killer Bees!-intro); O2-palet ≠ VP+-palet (VP+-palet uit RGB-encoder-specs).

**Systeem:** 64B interne + **128B** externe RAM (niet 256); vier BIOS-varianten via CRC herkend (O2ROM/C52/G7400/JOPAC — sommige O2-games draaien juist níét op VP+-BIOS); reset-toets (warm) is een andere toestand dan power-cycle (koud). **Audio:** 8244-toongenerator + witte ruis (LFSR) + laagdoorlaatfilter dat de analoge uitgang nabootst (testcases: Frogger, Popeye). **The Voice:** in O2EM pragmatisch als WAV-sample-playback (`voice/XXYY.wav`) — bewezen fase-1-model; echte SP0256-emulatie kan later.

**Belangrijkste meta-les (letterlijk citaat auteur):** *"the timing in o2em is so wrong, that a lot of problems with games arised"* — O2EM's 238-entry hack-database (`regionoff`, `evblclk=7642`, `pendirq`, `dishirq`, `mxsnap`, …) bestaat als **compensatie voor foute basistiming**. Ontwerpprincipe voor VideopacHorse_Core: **cycle-accuracy vanaf dag één, nul per-game hacks**; de hack-lijst is wél de perfecte regressie-testset (de moeilijke games: Atlantis, Frogger, Popeye, Q*bert, Pick Axe Pete, Four in 1 Row, Great Wall Street). O2EM-gedrag is **nooit** ground truth — alleen datasheets (8048, 8244/8245, EF9340/41) en echt ijzer.

## 5. Homebrew-testtools (roms/hb-tools/) — nuttig maar niet vrij herdistribueerbaar

Alle vier van **Mark Guttenbrunner ("Manopac")**, elk 2KB, signatuur in de binary; .a48-bronnen openbaar op guttenbrunner.com/videopac maar **zonder licentieverklaring** (sndtest/sp_build dragen expliciet "Copyright (C) 2006"):

| ROM | Functie | Waarde voor ons |
|---|---|---|
| `sndtest.bin` | interactief 8244-geluidsregisters verkennen | audio-registergedrag vergelijken |
| `gridedit.bin` | grid-registers (dots/fill/kleur) tekenen | grid-subsysteem oefenen |
| `sp_build.bin` | 2-char-sprites bouwen — auteur waarschuwt zelf: **O2EM rendert char-overlap anders dan echte hardware** | gouden testcase voor hardware-accurate overlap |
| `keytest7400.rom` | G7400-keyboardtest | pas relevant bij VP+-fase |

Ze bevestigen en passant de cart-conventie: reset-entry `$400`, IRQ-doorverwijzing `$402`, `CALL $0F1` de BIOS in (→ draaien niet zonder BIOS-image). **Advies:** niet in de publieke repo (toestemming vragen kan — Manopac is actief op videopac.nl t/m 2023); voor CI eigen deterministische, self-checking 8048-test-ROMs schrijven (Manopacs .a48-boilerplate als referentie), want deze tools zijn interactief, niet self-checking.

## 6. Juridische samenvatting

1. **O2EM = Clarified Artistic License**: gedrag/feiten bestuderen is vrij; **code of tabellen kopiëren maakt VideopacHorse CAL-plichtig** → 100% from scratch bouwen, dan is de eigen licentie vrij te kiezen. Herkomst-verantwoording ("alleen gedrag/datasheets als bron") in de repo documenteren.
2. **Nooit in de repo:** BIOS-ROMs, game-ROMs, Manopac-tools, Voice-samples. Wel: hashes, CRC-database als feiten, eigen test-ROMs.
3. De 8244-charset is formeel chip-maskdata (grijs gebied, laag risico) — herkomst documenteren, uit chip-documentatie opbouwen, niet uit O2EM-source.

## 7. Directe consequenties voor VideopacHorse_Core (spec-input)

1. Scanline-accurate VDC + cycle-getrouwe 8048 als fundament (geen frame-based shortcut).
2. Collision-detectie als apart, zwaar getest subsysteem (bitmatrix alle objectklassen).
3. Cart-API met de ene mapping-formule uit §3 + P1-live-banking + P1=0xFF-reset.
4. **Input-record/replay vanaf dag 1** (O2EM v1.21-idee) = fundament van de regressie-testsuite.
5. Savestate-header met BIOS-CRC + machinetype (O2EM-les: savestates zijn BIOS-gebonden).
6. Per-ROM CRC32-configdatabase (regio/mapper) — maar géén gedrags-hacks.
7. PAL/NTSC first-class runtime-parameter (T1-VBLANK-detectie door software).
8. Faseren: G7000-basis eerst; VP+ (EF9340/41), C7420 (Z80-in-cart), The Voice als latere modules.
9. Testverificatie op de Mac: RetroArch met libretro-o2em-core (native arm64) als referentie-runner, niet de Win32-exe.

## Bronverantwoording
Per-agent details met regel-/offsetverwijzingen: workflow `wf_d14dcdd0-fee`, volledige output in `tasks/wpar68zy4.output` (5 agents: manual, licentie+changelog, homebrew-roms, game-roms, binaries+config). Externe verificatie o.a. MAME `src/mame/philips/odyssey2.cpp` + `src/devices/bus/odyssey2/rom.cpp`, libretro No-Intro dat-mirrors, guttenbrunner.com/videopac.
