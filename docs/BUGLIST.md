# BUGLIST.md — VideopacHorse_Core

**Doel:** alle bugs van dit project + preventieregels zodat ze niet terugkeren.
**Onderhoud:** bij elke bugfix verplicht bijwerken (zie `feedback_debug_buglist_protocol`).

---

## Samenvatting

| Status | Aantal |
|--------|--------|
| 🔴 Open | 0 |
| 🟠 Recurring (terugkerend patroon) | 0 |
| ✅ Closed | 1 |
| **Totaal** | **1** |

---

## Terugkerende patronen (escalatie-zone)

Bugs die ≥2× zijn voorgekomen krijgen hier een eigen entry met **harder borgingsmechanisme** (hook, test, checklist-item). Zie ook `Meta_Master/BUGS_GLOBAL.md` voor cross-repo patronen.

_Geen terugkerende patronen vastgesteld._

---

## Open bugs

_Geen open bugs._

---

## Closed bugs

### BUG-001 — BIOS-scherm toont verhaspelde bonte tekens i.p.v. "SELECT GAME"

- **Datum melding:** 2026-07-26
- **Kleur:** 🟡 geel
- **Categorie:** rendering (VDC char/quad-tekenpad)
- **Status:** closed
- **Symptoom (wat zag de gebruiker):**
  - Het BIOS-opstartscherm toont een verhaspelde reeks bonte tekens op de
    plek waar "SELECT GAME" hoort te staan (live-melding gebruiker
    2026-07-26 via ClaudeBug).
- **RCA — 3 niveaus:**
  - **Functioneel:** chars en quad-chars renderden de verkeerde glyphs op
    de verkeerde plekken; alle BIOS-tekst was onleesbaar.
  - **Technisch:** drie fouten in het char/quad-pad van `src/vdc8244.c`:
    (1) de charset-index was puur relatief (rij = (lijn−Y)>>1 vanaf ptr)
    terwijl echt ijzer de ABSOLUTE Y meetelt — offset = ptr + (y>>1) +
    ((lijn−y)>>1), 9-bit wrap — en software (de BIOS) zijn pointers
    daarvoor compenseert; (2) de hoogte-afkap ontbrak: hoogte =
    7 − (((y>>1)+ptr)&7), waarde 0 → 8, zichtbaar y ≤ lijn < y+2·hoogte;
    (3) quad-chars gebruikten de VERKEERDE X/Y-bron (laatste sub-char
    i.p.v. de eerste) en misten de kwartet-hoogte uit de pointer van de
    vierde sub-char. Bijkomend gedragsfeit: NTSC tekent chars/quads met
    y < 0x0E niet; PAL wel. Bron: MAME i8244.cpp draw_major (gedragsfeit
    — geen code overgenomen).
  - **Architectonisch:** de char-pointer/Y-interactie stond in QUIRKS.md
    noot 4 expliciet als ONZEKERE interpretatie ("relatief model") maar
    was alleen vastgeklikt met een test die diezelfde interpretatie
    toetste — er bestond geen test die de software-compensatie-kant
    (BIOS-stijl gecompenseerde pointers) bewees. Een als onzeker
    gemarkeerde interpretatie zonder BIOS-stijl-test kon zo ongemerkt
    het hele tekstpad breken.
- **Oplossing:**
  - Commit: _nog niet gecommit (fix + testronde zelfde sessie 2026-07-26)_
  - Bestanden: `src/vdc8244.c` (`char_height`/`draw_char_slice`/
    `draw_chars`/`draw_quads`), `tests/test_vdc.c` (set_char-compensatie,
    herschreven V5/V7b/V_char_y_ptr_canon, nieuwe
    `test_V13_char_ptr_y_bios_compensatie`), `docs/QUIRKS.md`
    (noot 4 + V5-regel + nieuwe V13-regel).
- **Preventieregel (borging):**
  - Elke "interpretatie" in QUIRKS.md-noten krijgt een test die óók de
    software-compensatie-kant toetst (hoe echte software — BIOS/games —
    het gedrag gebruikt), niet alleen een test die de gekozen
    interpretatie zichzelf laat bevestigen.
  - Conform bestaande regel: quirk eerst in QUIRKS.md + test, dan
    implementatie (CLAUDE.md kernregel 6).
- **Detectie (vroege waarschuwing):**
  - `test_V13_char_ptr_y_bios_compensatie` (zelfde glyph via
    gecompenseerde pointers op 3 Y-posities → pixel-identiek + expliciete
    hoogte-afkap-case) faalt direct bij regressie van het absolute-Y-model.
  - Visuele smoke met echte BIOS (`G7K_BIOS`) zodra de BIOS-runner in de
    testflow zit: "SELECT GAME" leesbaar = canary.
- **Patroon-tags:** `vdc`, `rendering`, `interpretatie-zonder-bios-test`,
  `charset-adressering`
- **Verwante bugs:** —

---

## Bug-entry format (kopieer dit blok)

```markdown
### BUG-XXX — Korte titel

- **Datum melding:** YYYY-MM-DD
- **Kleur:** 🟢 groen / 🟡 geel / 🟠 oranje / 🔴 rood
- **Categorie:** deploy / cache / rendering / scoring / auth / data / build / ...
- **Status:** open / closed / recurring
- **Symptoom (wat zag de gebruiker):**
  - …
- **RCA — 3 niveaus:**
  - **Functioneel:** wat brak vanuit gebruikersperspectief?
  - **Technisch:** welke code/config/API veroorzaakte het?
  - **Architectonisch:** moet het systeemontwerp wijzigen?
- **Oplossing:**
  - Commit: `<hash>` — `<korte boodschap>`
  - Bestanden: `path/to/file.ext`, …
- **Preventieregel (borging):**
  - Wat doen we voortaan anders zodat deze bug niet terugkomt?
  - Verwijs naar memory/feedback regel indien al bestaand.
- **Detectie (vroege waarschuwing):**
  - Hoe ontdekken we het volgende keer vroeger? (test, hook, checklist-item, monitoring)
- **Patroon-tags:** `tag1`, `tag2` — gebruik voor cross-link
- **Verwante bugs:** BUG-YYY (zelfde patroon), `Meta_Master/BUGS_GLOBAL.md#PATTERN-ID`
```

---

## Workflow

1. **Bij elke nieuwe bug:** entry toevoegen onder "Open bugs"
2. **Na fix:** verplaatsen naar "Closed bugs" + status `closed` + commit-hash invullen
3. **Bij ≥2× zelfde patroon:** entry naar "Terugkerende patronen" + harder borgingsmechanisme verzinnen
4. **Voor elke deploy:** sectie "Terugkerende patronen" doorlopen → preventieregels actief checken
5. **Cross-repo patroon:** ook toevoegen aan `Meta_Master/BUGS_GLOBAL.md`
