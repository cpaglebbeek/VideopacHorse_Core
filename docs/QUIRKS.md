# QUIRKS.md — hardware-gedragscanon VideopacHorse_Core

> Bron: `docs/O2EM_DEEPDIVE.md` (ultracode-analyse 2026-07-26 van O2EM 1.21-docs/changelog,
> MAME-broncode-feiten en eigen ROM-metingen). Elk item hieronder heeft in O2EM aantoonbaar
> een game gebroken en MOET door een expliciete test in `tests/` worden afgedekt.
> Status-kolom wordt bijgewerkt door de bouw-workflow.

## CPU — Intel 8048 (MCS-48)

| # | Gedrag | Canary | Test |
|---|---|---|---|
| C1 | Conditionele branches blijven binnen de 256-byte pagina | Sid the Spellbinder | [x] |
| C2 | 11-bit PC wrapt op 0x7FF naar 0x000 | (O2EM v1.18.1) | [x] |
| C3 | DAA correct (BCD-scores) | Le Tresor Englouti, Cosmic Conflict | [x] |
| C4 | JNI-instructie aanwezig en correct | (O2EM v0.90) | [x] |
| C5 | Externe interrupt → vector 0x003, correcte state | Killer Bees | [x] |
| C6 | Cycle-getrouwe instructietimings (1/2-cycle per datasheet) | (O2EM v1.15) | [x] |
| C7 | T1-ingang = VBLANK ÓF HBLANK (beam-blank); software meet zo de regio-timing (PAL/NTSC-detectie) | (O2EM v1.01) | [x] |
| C8 | P1 = 0xFF na reset (MCS-48-datasheet) → 8K-cart boot uit bank 3 | vp_01pl (CRC EE3EE642) | [x] |
| C9 | MB0/MB1-bankbit + JMP/CALL over 2K-grens | — | [x] |

## Geheugen & cartridge

| # | Gedrag | Bron | Test |
|---|---|---|---|
| M1 | 1KB BIOS op 0x000-0x3FF (in de 8048), cart op 0x400-0xBFF | MAME odyssey2.cpp | [x] |
| M2 | 0xC00-0xFFF spiegelt 0x800-0xBFF (A10 niet aangesloten) | MAME rom.cpp | [x] |
| M3 | bank = P1 & 3, live bij ELKE P1-write (geen mapper-register) | MAME rom.cpp | [x] |
| M4 | mapping: ((addr>=0xC00?addr-0x800:addr-0x400)+bank*0x800)&(bit_floor(size)-1) | MAME rom.cpp | [x] |
| M5 | 64B interne RAM; 128B externe RAM (NIET 256) | O2EM v1.18.1 | [x] |
| M6 | Out-of-range cart-read → 0xFF (open bus) | MAME | [x] |
| M7 | Testvectoren: na reset eerste cart-fetch @0x400 = 0x44 (vp_01pl: file[0x1800]; vp_14/24: file[0]); bank-CRC's 004DEC9C/576D1FE3/0EBC9913/13EB8DD9 | eigen meting | [x] |

## VDC — Intel 8244/8245

| # | Gedrag | Canary | Test |
|---|---|---|---|
| V1 | Scanline-accuraat renderen; mid-screen registerwrites zonder IRQ | Power Lords, Super Cobra | [x] |
| V2 | VDC-writes GEBLOKKEERD zolang foreground enabled (echt ijzer) | Puzzle Piece Panic "emu-versie" = anti-testcase | [x] |
| V3 | Collision-bitmatrix alle objectklassen (sprites/chars/grid/dots) | Killer Bees, Demon Attack, Cosmic Conflict | [x] |
| V4 | Double-size sprites schuiven per 2 pixels | (O2EM 1.17.2/1.18/1.18.1) | [x] |
| V5 | Quad-chars eigen tekenpad | KTAA, Black Hole/Red Baron-scores | [x] |
| V6 | Sprite/char-prioriteitsvolgorde | Demon Attack, Turtles, Atlantis | [x] |
| V7 | Geen wrap van sprites/chars aan schermranden | Atlantis, P.T. Barnum's Acrobats | [x] |
| V8 | Shifted/smooth sprite-modus (pseudo-hi-res) | Q*bert | [x] |
| V9 | 64-tekens charset: VOLGORDE uit chip-documentatie ([B] App. C), glyph-VORMEN eigen ontwerp (NIET uit O2EM/MAME; echte maskdata niet publiek — zie noot 10) | — | [x] |
| V10 | Dot-grid incl. rechterkolom; correcte gridlijn-hoogtes + onderste lijn | Marksman | [x] |
| V11 | P17 laag = alle VDC-kleuren fel (achtergrond + donkere gridkleuren) | Killer Bees!-intro | [x] |
| V12 | Leesgedrag unused/foreground/grid-registers zoals echt ijzer | (O2EM v1.18.1) | [x] |

## Audio (in de 8244)

| # | Gedrag | Canary | Test |
|---|---|---|---|
| A1 | Shift-register toongeneratie, correcte pitch | Frogger, Popeye | [x] |
| A2 | Witte-ruis (LFSR) voor explosies | — | [x] |
| A3 | Optioneel laagdoorlaatfilter (analoge uitgang) | — | [x] |

## Systeem

| # | Gedrag | Test |
|---|---|---|
| S1 | Warme reset (reset-toets) ≠ koude reset (power-cycle wist RAM/VDC) | [x] |
| S2 | PAL 50Hz / NTSC 60Hz first-class runtime-parameter | [x] |
| S3 | Savestate-header bevat BIOS-CRC + machinetype + regio | [x] |
| S4 | Keyboard-matrix-scan zoals BIOS hem uitvoert | [ ] |

## Teststatus & beargumenteerd uitstel (integratie v0.1, 2026-07-26)

Alle [x]-items zijn gedekt door groene tests in `make test` (ASan/UBSan):
`tests/test_cpu.c` (C1-C9), `tests/test_cart.c` (M1-M4, M6, M7),
`tests/test_vdc.c` (V1-V12, A1-A3, C7-VDC-kant, fill/HIRQ/beam-latch/palet),
`tests/test_state.c` (S3), `tests/test_sys.c` (M5, S1, S2, C7-bedrading
end-to-end via `test_C7_t1_wiring_vbl_hbl`). Kanttekeningen:

1. **S4 open (enige [ ]).** De teken→matrixcode-tabel (`g7k_key_from_char`)
   levert nog `G7K_KEY_NONE` en de joystick/keyboard-bitvolgorde in `sys.c`
   is een documentatie-aanname. Bewust uitgesteld: de tabel hoort afgeleid
   én getest te worden tegen de echte BIOS-scanroutine (S4-bouwer), geen
   goktabel in het contract (ARCHITECTURE.md appendix, open punt 11).
2. **M7 hardware-vectoren.** De M4/M2/M3-mapping is groen bewezen op
   synthetisch nagebouwde vectoren (eerste fetch 0x44 + per-bank
   CRC-streams). `test_M7_real_rom_vp01pl` verifieert dezelfde vectoren
   tegen een echte `vp_01pl.bin` (CRC EE3EE642) en SKIPt zonder
   `G7K_ROMDIR` — geen ROMs in git (CLAUDE.md regel 3).
   **Echt gedraaid op 2026-07-26:** `G7K_ROMDIR=~/Downloads make`-run,
   lokale `vp_01pl.bin` CRC geverifieerd `EE3EE642`, resultaat
   `[PASS] test_M7_real_rom_vp01pl` (84/84 groen, 0 skip). In CI zonder
   ROM blijft de regel dus op synthetische vectoren + deze gedateerde
   hardware-run leunen.
3. **Timer-prescaler-fase (CPU).** STRT T reset de /32-prescaler; de
   datasheet laat ±1 cyclus marge. Deterministische keuze gedocumenteerd in
   `cpu8048.c` en vastgeklikt in `cpu_timer_prescaler_div32` — kalibratiepunt
   als audio/VDC-metingen ooit afwijken.
4. **VDC-interpretaties.** Expliciet gemarkeerde interpretaties in
   `vdc8244.c`, v0.1-canon, elk vastgeklikt in een test; herzien zodra
   echte-ijzer/BIOS-metingen beschikbaar zijn:
   - char-pointer/Y-interactie relatief model (rij=(lijn-Y)>>1; [B] 4.4
     noemt de echte interactie "very confusing") — `test_V_char_y_ptr_canon`;
   - V12-leeswaarden (geblokte/ongebruikte registers lezen 0x00);
   - sprite-bit0=links; fill-kleur=gridkleur ([B] 4.2 aangehouden waar
     4.2/4.6 elkaar tegenspreken) en fill-collision-klasse=verticale grid
     — `test_V_fill_mode_color_collision`;
   - dot-grid op alle kruispunten; grid-geometrie-canon;
     ruis-LFSR-polynoom x^15+x^14+1; beam-X=0 (geen intra-lijn-fase in
     $A5) — `test_V_beam_latch_a4_a5`;
   - char-overlap-status ($A1 bit 7) per scanlijn herbepaald (tegenwoordige
     tijd in [B] 4.7, geen eeuwige latch; tijdens VBLANK blijft de waarde
     van de laatste actieve lijn staan) — `test_V12b_strobe_hbl_chrovl`;
   - VBL-statusbit als latch-tot-statusread i.p.v. de 40µs-puls uit
     [B] 4.7 (contract-gebonden: core_internal.h "statusread cleart
     VBL-bit"; wijziging vereist architect-besluit) — `test_V12`;
   - HIRQ ($A0 bit 0) vuurt bij lijnSTART van actieve lijnen en niet
     tijdens VBLANK-lijnen ([B] 4.6 zegt "at each horizontal blank"
     zonder beperking; scanline-granulariteit maakt de fasekeuze
     noodzakelijk, VBLANK-gedrag onbekend) — `test_V_hirq_active_lines`;
   - HBL-statusbit ($A1 bit 0) intra-lijn via beam-fase die sys.c vóór
     elke registertoegang doorzet (`vdc8244_set_line_phase`) —
     `test_V12b_strobe_hbl_chrovl`.
5. **Sound-IRQ ($A0 bit 2)** is passief gemodelleerd (niet aan de
   8048-INT-lijn gekoppeld). Geen bekende game-afhankelijkheid; herzien
   bij de S4/echte-BIOS-testronde. Het $A1-strobe-bit is sinds 2026-07-26
   wél actief gemodelleerd met de [B] 4.7-polariteit (1 = follow beam,
   0 = latched) — `test_V12b_strobe_hbl_chrovl`.
6. **Palet-RGBA** volgt de digitale RGB-bitdecodering; benadering van de
   kleurnamen in Boris Appendix B, met drie gedeclareerde afwijkingen
   van de letterlijke namen: dim-achtergrond 3 ("Light Green") rendert
   als blauwgroen 0x00A0A0 (G+B-decodering), bright-achtergrond 0
   ("Black") rendert als donkergrijs 0x404040 (V11 "alles fel"),
   obj-kleur 6 ("Light Grey") rendert als cyaan (G+B-decodering).
   Alle 24 waarden zijn vastgeklikt in `test_V_palette_all24`; bij
   canonieke gemeten hexwaarden: alleen de twee palettabellen in
   `vdc8244.c` + de testtabellen aanpassen.
7. **state.c strikter dan de minimale S3-lijst:** load weigert ook bij
   cart-size-mismatch en ongeldige regio-waarde. Integratie-akkoord: dit is
   veiliger (blob past anders niet op de geladen cart) en blijft binnen het
   S3-contract "load weigert bij mismatch, state onaangetast".
8. **12K/16K-carts:** via bank=P1&3 is maximaal de eerste 8K bereikbaar
   (2K-venster × 4 banken; 12K met bit_floor-masker 0x1FFF). Echte
   12K/16K-boards (o.a. Trans American Rally) hebben vermoedelijk extra
   adreslijnen → aparte mappingvariant in een latere fase; geen per-game
   hack (ontwerpprincipe 1).
9. **MOVD/ANLD/ORLD (8243-expander):** geen 8243 op de G7000; MOVD A,Pn
   leest 0x0F (zwevende 4-bit bus), writes zijn no-ops. Gedocumenteerd in
   `cpu8048.c` voor het geval een cart-variant ooit een expander heeft.
10. **V9-glyphvormen zijn eigen ontwerp.** Alleen de 64-tekens-VOLGORDE
    komt uit [B] App. C; de echte 8244-maskdata (pixelvormen) is niet
    publiek gedocumenteerd en mag niet uit O2EM/MAME worden overgenomen
    (licentie-brandmuur). Games renderen dus leesbare maar niet
    ijzer-identieke tekens. `test_V9b_charset_all64_distinct` klikt alle
    64 eigen glyphs vast (spatie leeg, rest uniek) als regressieanker;
    vervangen zodra een rechtenvrije glyph-dump of eigen meting van echt
    ijzer beschikbaar is.
11. **P16-gate op externe writes (2026-07-26, review-finding).** De
    externe RAM-write-strobe is alleen actief als P16 (P1 bit 6) laag is
    (gedragsfeit bron [1] io_write); de leeskant had die gate al. Writes
    met P16 hoog verdwijnen in het niets — `test_M5_extram_128_not_256`
    dekt de spook-write. De VDC-write hangt NIET achter deze gate (alleen
    P13). Contractregel in `core_internal.h` (SYS, MOVX-write) is
    gelijkgetrokken; formele architect-ratificatie van die contracttekst
    staat open.

## Ontwerpprincipes (uit de deep-dive)

1. **Nul per-game hacks.** O2EM's 238-entry CRC-hackdatabase is compensatie voor foute
   basistiming; wij lossen de oorzaak op. De hack-gamelijst (Atlantis, Frogger, Popeye,
   Q*bert, Pick Axe Pete, Four in 1 Row, Great Wall Street) is onze regressie-testset.
2. **O2EM is nooit ground truth** ("the timing in o2em is so wrong" — de auteur).
   Alleen datasheets (8048, 8244/8245) en gemeten ROM-feiten.
3. **Geen ROMs in git.** BIOS/game/homebrew-ROMs alleen via gebruiker; hashes als fixtures.
4. **Input-record/replay vanaf dag 1** als fundament van deterministische regressietests.
