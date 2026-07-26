# QUIRKS.md — hardware-gedragscanon VideopacHorse_Core

> Bron: `docs/O2EM_DEEPDIVE.md` (ultracode-analyse 2026-07-26 van O2EM 1.21-docs/changelog,
> MAME-broncode-feiten en eigen ROM-metingen). Elk item hieronder heeft in O2EM aantoonbaar
> een game gebroken en MOET door een expliciete test in `tests/` worden afgedekt.
> Status-kolom wordt bijgewerkt door de bouw-workflow.

## CPU — Intel 8048 (MCS-48)

| # | Gedrag | Canary | Test |
|---|---|---|---|
| C1 | Conditionele branches blijven binnen de 256-byte pagina | Sid the Spellbinder | [ ] |
| C2 | 11-bit PC wrapt op 0x7FF naar 0x000 | (O2EM v1.18.1) | [ ] |
| C3 | DAA correct (BCD-scores) | Le Tresor Englouti, Cosmic Conflict | [ ] |
| C4 | JNI-instructie aanwezig en correct | (O2EM v0.90) | [ ] |
| C5 | Externe interrupt → vector 0x003, correcte state | Killer Bees | [ ] |
| C6 | Cycle-getrouwe instructietimings (1/2-cycle per datasheet) | (O2EM v1.15) | [ ] |
| C7 | T1-ingang = VBLANK-puls; software detecteert zo PAL/NTSC | (O2EM v1.01) | [ ] |
| C8 | P1 = 0xFF na reset (MCS-48-datasheet) → 8K-cart boot uit bank 3 | vp_01pl (CRC EE3EE642) | [ ] |
| C9 | MB0/MB1-bankbit + JMP/CALL over 2K-grens | — | [ ] |

## Geheugen & cartridge

| # | Gedrag | Bron | Test |
|---|---|---|---|
| M1 | 1KB BIOS op 0x000-0x3FF (in de 8048), cart op 0x400-0xBFF | MAME odyssey2.cpp | [ ] |
| M2 | 0xC00-0xFFF spiegelt 0x800-0xBFF (A10 niet aangesloten) | MAME rom.cpp | [ ] |
| M3 | bank = P1 & 3, live bij ELKE P1-write (geen mapper-register) | MAME rom.cpp | [ ] |
| M4 | mapping: ((addr>=0xC00?addr-0x800:addr-0x400)+bank*0x800)&(bit_floor(size)-1) | MAME rom.cpp | [ ] |
| M5 | 64B interne RAM; 128B externe RAM (NIET 256) | O2EM v1.18.1 | [ ] |
| M6 | Out-of-range cart-read → 0xFF (open bus) | MAME | [ ] |
| M7 | Testvectoren: na reset eerste cart-fetch @0x400 = 0x44 (vp_01pl: file[0x1800]; vp_14/24: file[0]); bank-CRC's 004DEC9C/576D1FE3/0EBC9913/13EB8DD9 | eigen meting | [ ] |

## VDC — Intel 8244/8245

| # | Gedrag | Canary | Test |
|---|---|---|---|
| V1 | Scanline-accuraat renderen; mid-screen registerwrites zonder IRQ | Power Lords, Super Cobra | [ ] |
| V2 | VDC-writes GEBLOKKEERD zolang foreground enabled (echt ijzer) | Puzzle Piece Panic "emu-versie" = anti-testcase | [ ] |
| V3 | Collision-bitmatrix alle objectklassen (sprites/chars/grid/dots) | Killer Bees, Demon Attack, Cosmic Conflict | [ ] |
| V4 | Double-size sprites schuiven per 2 pixels | (O2EM 1.17.2/1.18/1.18.1) | [ ] |
| V5 | Quad-chars eigen tekenpad | KTAA, Black Hole/Red Baron-scores | [ ] |
| V6 | Sprite/char-prioriteitsvolgorde | Demon Attack, Turtles, Atlantis | [ ] |
| V7 | Geen wrap van sprites/chars aan schermranden | Atlantis, P.T. Barnum's Acrobats | [ ] |
| V8 | Shifted/smooth sprite-modus (pseudo-hi-res) | Q*bert | [ ] |
| V9 | 64-tekens charset (uit chip-documentatie, NIET uit O2EM-source) | — | [ ] |
| V10 | Dot-grid incl. rechterkolom; correcte gridlijn-hoogtes + onderste lijn | Marksman | [ ] |
| V11 | P17 laag = alle VDC-kleuren fel (achtergrond + donkere gridkleuren) | Killer Bees!-intro | [ ] |
| V12 | Leesgedrag unused/foreground/grid-registers zoals echt ijzer | (O2EM v1.18.1) | [ ] |

## Audio (in de 8244)

| # | Gedrag | Canary | Test |
|---|---|---|---|
| A1 | Shift-register toongeneratie, correcte pitch | Frogger, Popeye | [ ] |
| A2 | Witte-ruis (LFSR) voor explosies | — | [ ] |
| A3 | Optioneel laagdoorlaatfilter (analoge uitgang) | — | [ ] |

## Systeem

| # | Gedrag | Test |
|---|---|---|
| S1 | Warme reset (reset-toets) ≠ koude reset (power-cycle wist RAM/VDC) | [ ] |
| S2 | PAL 50Hz / NTSC 60Hz first-class runtime-parameter | [ ] |
| S3 | Savestate-header bevat BIOS-CRC + machinetype + regio | [ ] |
| S4 | Keyboard-matrix-scan zoals BIOS hem uitvoert | [ ] |

## Ontwerpprincipes (uit de deep-dive)

1. **Nul per-game hacks.** O2EM's 238-entry CRC-hackdatabase is compensatie voor foute
   basistiming; wij lossen de oorzaak op. De hack-gamelijst (Atlantis, Frogger, Popeye,
   Q*bert, Pick Axe Pete, Four in 1 Row, Great Wall Street) is onze regressie-testset.
2. **O2EM is nooit ground truth** ("the timing in o2em is so wrong" — de auteur).
   Alleen datasheets (8048, 8244/8245) en gemeten ROM-feiten.
3. **Geen ROMs in git.** BIOS/game/homebrew-ROMs alleen via gebruiker; hashes als fixtures.
4. **Input-record/replay vanaf dag 1** als fundament van deterministische regressietests.
