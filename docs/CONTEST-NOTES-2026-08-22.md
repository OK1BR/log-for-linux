# Poznámky z reálného nasazení — závod 2026-08-22 (YO DX HF)

Ostré contest-nasazení. Testované binárky (žádná není z instalace v
`~/.local`, jede se z build stromů):

- **log-for-linux** — `builddir/log-for-linux`, build 21. 8. 14:44,
  git `1888ab4` „contest: YO DX HF preset (ADIF YOHFDX, Cabrillo YO-DX-HF)"
- sdr-for-linux 0.4.1 — `build/sdr-for-linux` (dev build), git `1c9761c`
- skimmer-for-linux 0.3.0 — `builddir/skimmer-for-linux`, git `107172f`

Režim: Richard hlásí poznámky za provozu, **žádné zásahy do kódu během
závodu** — jen zápis + vyhodnocení. Triage do verzí až po závodě.

Formát: každá položka = co se stalo (Richardova slova) → vyhodnocení
(závažnost, příčina, kde v kódu) → návrh opravy pro příští verzi.

⚠️ **Čísla řádků v tomhle dokumentu platí pro `1888ab4`** (binárka, na které
se závod jel). Od té doby se `win.c` posunul — aktuální ukazatele drží
`docs/BACKLOG.md`, který je živý; tenhle dokument je datovaný záznam.

---

## Poznámky

### 1. Pole Nr/County nepřepíná psaní na velká písmena
**Hlášení:** „při zapisování spojení v poli nr/country by měli jít psát
country code výhradně velkými písmeny… týká se yo dx závodu."

**Vyhodnocení:** závažnost **vadí** (ergonomie za provozu, ne datová
vada). Jde o pole `Nr/County` z YO DX presetu — `contest.c:476`, jedno
AUTO pole nese obojí (naše RST+serial ven, od YO stanic okresní zkratka
typu `BU`).

Data v logu jsou v pořádku: `logfl_exch_apply()` hodnotu při zápisu do
QSO uppercasuje — `contest.c:291` (`g_ascii_strup` na přijatou hodnotu)
a `contest.c:279` (totéž na `my_exch`). Stejně tak dodatečná editace
buňky v tabulce, `parse_exch_cell()` v `win.c:4102`. Do ADIF i Cabrilla
tedy county code odchází velkými bez ohledu na to, jak se natukl.

Vada je čistě v tom, **co operátor vidí v poli za provozu**. Živý
uppercase při psaní zajišťuje `entry_force_upper()` (`win.c:241`), ale
je připojený jen na dvě místa:

- `win.c:5156` — volačka v zadávacím řádku (`self->call`),
- `win.c:4638` — buňka tabulky, a to jen `if (col == COL_CALL)`.

Exchange pole se staví v `rebuild_exch_fields()` a vzniká holým
`mk_entry()` bez uppercase hooku — `win.c:2836`. Totéž `self->serial_value`
(pole „Sent") na `win.c:2828`.

Je to přesně ta mezera, kterou u volačky uzavřel SCOPE 2026-08-14 s
odůvodněním v komentáři na `win.c:214`: *„a hand-typed callsign shows in
capitals AS it is typed, whatever Caps Lock happens to be doing — the
operator never looks at the keyboard mid-QSO."* Argument platí pro
exchange doslova stejně: v závodě se do pole tuká naslepo, s Caps Lockem
v neznámém stavu, a operátor potřebuje vidět, co reálně odejde.

**Návrh opravy (příští verze):** zavolat `entry_force_upper (e)` v
`rebuild_exch_fields()` hned po `mk_entry()` na `win.c:2836`, a stejně
tak na `self->serial_value` (`win.c:2828`). Bez podmínky na typ pole —
`g_utf8_strup` je na číslicích no-op, takže `serial`/`number` pole to
neovlivní a odpadá větvení. Dvouřádková změna, ověřit živě natukáním
`bu` do Nr/County (má naskočit `BU`) a kontrolou, že serial prefill
a mazání pole po zalogování dál fungují.

**Rozsah:** doporučuju vzít i do `COL_*` buněk tabulky pro exchange
sloupec — `win.c:4638` dnes uppercasuje jen `COL_CALL`, takže tabulková
editace exchange sice uloží velkými (`parse_exch_cell`), ale během
psaní ukazuje malá. Stejná vada, stejná jednořádková oprava.

### 2. Počítání bodů a multiplikátorů, mult jako sloupec v logu
**Hlášení:** „u logu by se hodilo vymyslet mechanismus počítání bodů…
a je třeba začít u závodů počítat s multiplikátory, možná je zvýraznit
do nějakého sloupce v logu."

**Vyhodnocení:** závažnost **chybějící funkce**, ne vada. Z velké části
už zaevidované — `docs/SCOPE.md:387`, *„Contest score calculation. IDEA
(Richard, 2026-08-08)"*: live claimed score v contest UI + Cabrillo
CLAIMED-SCORE, per-contest QSO body (bodové tabulky jednotlivých presetů
už zrešeršované a ozdrojované z oficiálních pravidel 8. 8.) a multiplier
tracking po pásmech. Dnešní hlášení proti tomu přináší dvě věci:

**a) Priorita.** Z nápadu na papíře se stal požadavek z ostrého provozu.
To je pro triage podstatnější než technický obsah — dnes se skóre počítá
ručně, Cabrillo `CLAIMED-SCORE` je v `cabrillo.h:31` prostý „integer
text", tj. operátor ho vyplňuje z hlavy.

**b) Nové proti IDEA: multiplikátor viditelný per QSO.** IDEA počítá
s multiplikátory jako s *počítadlem* po pásmech; Richard chce navíc
u konkrétního spojení v tabulce vidět, že přineslo nový násobič. To ve
SCOPE není. Infrastruktura pro to existuje: `LOGFL_QSO_ZERO_POINTS`
už v komentáři nese pojem „counts, but 0 QSO points (still a mult)"
(`contest.h:107`), cty resolver dodává DXCC entitu, prefix, kontinent
i zóny (`cty.h:28`), a sloupce tabulky jsou přímočaře rozšiřitelné —
enum `COL_*` na `win.c:49` a `add_column()` volání na `win.c:5002–5019`.
Dnešní sloupce: UTC, Band, Mode, MHz, Call, RST s/r, Sent, Rcvd, Name,
Comment — žádný bodový ani mult.

**c) YO DX jako živý příklad mezery.** Bodová struktura tohoto závodu
(8 b. YO / 4 jiný kontinent / 2 stejný kontinent / 1 stejná DXCC) je
dnes zapsaná **jen jako komentář** v `contest.c:465`, ne strojově —
preset nese `counts` a `zero_own_country` (tj. jen *platnost* QSO),
žádnou bodovou hodnotu. Jakýkoli mechanismus bodování si vyžádá rozšíření
serializace `exch_def` o něco jako `points=`, a YO DX je zrovna ten
případ, na kterém to bude vidět.

**Návrh (příští verze):** rozšířit stávající IDEA ve `SCOPE.md:387`
o per-QSO mult sloupec v tabulce a o strojově čitelné bodové pravidlo
v `exch_def`; zbytek už tam popsaný je. Do kterého milníku to spadne
a jestli se dělá celé najednou nebo po částech — rozhodnout v triage po
závodě, ne teď.

### 3. Po přeladění / přepnutí volačky zůstane viset staré číslo spojení
**Hlášení:** „závada v deníku, při přeladění správně zmizne volačka, nebo
se volačka přepne na novou, ale zůstane tam viset starý, nebo blbě
vyplněný číslo spojení."

**Vyhodnocení:** závažnost **vážné** — do QSO se špatnou stanicí odejde
cizí přijatá výměna, což je chyba v deníku, ne kosmetika. Příčina
nalezena čtením kódu, jde o **dvě různá místa**; obě sedí na popis.

**Cesta A — klik ze spotu na spot (nejpravděpodobnější).** Značka
„volačka je nedotčený prefill ze spotu" (`call_from_spot`, `win.c:86`)
se shazuje handlerem `on_call_changed_drop_spot` (`win.c:492`), jenže
ten je připojený **jedině na pole Call** — `win.c:5160`, jediné volání
v souboru. Exchange pole vzniklá v `rebuild_exch_fields()`
(`win.c:2834–2842`) nemají navěšený žádný signál. Důsledek:

1. klik na spot A → volačka `OK1ABC`, `call_from_spot = TRUE`
2. operátor natuká přijatou výměnu (`BU`, číslo) → značka **zůstane TRUE**,
   protože editace exchange ji neshazuje
3. klik na spot B → podmínka na `win.c:525` je splněná, volačka se
   přepíše na `OK2XYZ`
4. `tci_apply_spot()` (`win.c:521–540`) ale zapisuje **jen volačku** —
   výměna od `OK1ABC` zůstane v poli viset

Přitom sousední QSY větev to dělá správně: `tci_apply_state()` na
`win.c:452` volá `entry_reset_defaults()`, tedy celý řádek, s výslovným
odůvodněním v komentáři *„typed RST/exchange/name of a QSO that never
happened must not haunt the next station."* Stejný požadavek u spot→spot
větve nikdo neuplatnil.

**Cesta B — QSY s otevřeným editorem buňky.** Tamtéž, `win.c:453–455`:
když je rozeditovaná buňka v tabulce (`self->cell_edit_box`), reset se
vědomě zúží na `gtk_editable_set_text (self->call, "")`, aby se
nepřetahoval fokus. Volačka tedy zmizí — přesně jak Richard popisuje —
ale RST, jméno i exchange zůstanou. Výjimka je záměrná kvůli fokusu,
ale její dopad na data zjevně promyšlený nebyl.

**Okrajově:** práh pro zahození prefillu je `LOGFL_SPOT_KEEP_HZ = 200.0`
(`win.c:408`). Přeladění o méně než 200 Hz — v CW pileupu běžné — se
za „odladění" nepovažuje a nezahodí nic. Zmiňuju pro úplnost; na
hlášené chování to sedí hůř než A a B.

**Návrh opravy (příští verze):** připojit `on_call_changed_drop_spot`
i na exchange pole (a na RST/Name) v `rebuild_exch_fields()`. Tím se
značka `call_from_spot` shodí, jakmile operátor začne QSO psát — v duchu
pravidla, které v kódu už stojí: *„a call the operator typed is theirs
and stays."* Spot→spot pak volačku přepíše jen dokud je řádek opravdu
nedotčený, a viset nemá co zůstat. Samostatně a nezávisle: ve větvi
s otevřeným editorem buňky (`win.c:454`) vyčistit i zbytek řádku, jen bez
`grab_focus` — fokus zůstane v buňce, data se nezanesou.

**K ověření po opravě:** spot A → natukat výměnu → spot B (výměna musí
zmizet); QSY o >200 Hz s rozeditovanou buňkou (řádek se má vyčistit,
fokus zůstat v buňce); a kontrola, že ruční editace výměny nadále
nezpůsobí, že by spot přepsal natukanou volačku.

---

# 2. den — 23. 8. 2026 (a export po závodě)

Binárka **beze změny** (`builddir/log-for-linux`, git `1888ab4`, build
21. 8. 14:44). Spuštěno 12:23, 18 QSO (celkem za závod 107, vše 20 m CW).
stderr za celý 2. den: **prázdný** (`/var/tmp/contest-2026-08-23-logy/log.log`,
0 B) — žádný warning, critical ani assert.

## Ověření exportů (23. 8. 14:09, `~/Downloads/`)

### ADIF `ok1br-log.adi` — v pořádku
107× `<EOR>`, souhlasí s DB (`contest_ref=4` → 107 QSO). Hlavička
`<ADIF_VER:5>3.1.4`, `<PROGRAMID:13>log-for-linux`. Contest id `YOHFDX`
u každého QSO. Výměna: naše vždy `<STX>` (serial, číselně), přijatá od
non-YO stanic `<SRX>`, od YO stanic `<SRX_STRING>` s okresní zkratkou.

**Tím se potvrzuje tvrzení z položky #1** („data v logu jsou OK, vada je
jen vizuální") — už ne čtením kódu, ale proti reálné DB a exportu. Všech
14 přijatých county codes je v DB i v ADIF **velkými písmeny**:

```
YR8E BT · YP3X SV · YR7A GR · YO8OLY NT · YO7CW AG · YO6KNE HR · YR2X AR
YO6KPT CV · YO9KAG PH · YO2MNZ HD · YO4NF CT · YO3GCL BU · YO3LW BU · YR8D SV
```

`stx_string` je u všech 107 QSO prázdný, `stx` nese serial — správně.

### 4. Cabrillo `ok1br.log` — ⛔ špatná hlavička `CATEGORY-MODE: RTTY`
**Nález (ne hlášení).** Vyexportovaný Cabrillo obsahuje

```
CATEGORY-MODE: RTTY
```

přičemž **všech 107 QSO řádků je `CW`**. Zbytek souboru je v pořádku:
107× `QSO:`, `CONTEST: YO-DX-HF`, `CALLSIGN: OK1BR`, `GRID-LOCATOR: JO60TD`,
`CATEGORY-BAND: 20M` (odpovídá — jelo se jen 20 m), `END-OF-LOG:`.

**Závažnost: vážné / časově kritické.** Není to kosmetika v našem kódu —
je to soubor, který jde do soutěžního robota. S touhle hlavičkou by log
spadl do RTTY kategorie.

**Příčina — dohledána a ověřena.** Hodnota se **pamatuje z posledního
exportu** a přebíjí default:

- `settings.c:32` — default je `"MIXED"`,
- `win.c:4005` — combo v exportním dialogu se plní ze `self->settings.cab_mode`,
  ne z defaultu,
- `win.c:3891` — po exportu se zvolená hodnota uloží zpět.

Reálný stav `~/.config/log-for-linux/settings.ini`:

```
[cabrillo]
mode=RTTY
```

zůstalo z **SARTG WW RTTY 2026** (preset `id=3` v tabulce `contest`),
tedy z předchozího závodu. Dialog to nabídl předvyplněné a při YO DX to
prošlo bez povšimnutí.

**Návrh opravy (příští verze):** `CATEGORY-MODE` (a stejně tak
`CATEGORY-BAND`) **odvodit z reálně zalogovaných QSO** daného závodu —
distinct `mode` nad `contest_ref`: jeden mód → ten mód, víc módů →
`MIXED`; distinct `band` → jedno pásmo nebo `ALL`. Zapamatovaná hodnota
ať slouží nejvýš jako fallback, když v závodě ještě žádné QSO není.
Odvozená hodnota je vždy pravdivější než to, co si operátor pamatoval
z minulého závodu — a tady je vidět, že se na paměť spolehnout nedá.

**Nedotčeno:** exportovaný soubor v `~/Downloads/` ani `settings.ini`
jsem NEupravoval — čeká to na Richardovo rozhodnutí (odeslat znovu /
opravit hlavičku).

**Neověřeno:** `LOCATION: DX` a `CATEGORY-ASSISTED: ASSISTED` jsem proti
oficiálním pravidlům YO DX nekontroloval — pokud se bude reexportovat,
stojí za to projet celou hlavičku proti pravidlům.

---

## #4 — OPRAVENO 23. 8. (Richardovo zadání po závodě)

Richard hlavičku odeslaného logu opravil ručně a log poslal znovu; k appce
řekl, že *„bude to potřeba asi i ošetřit v nastavení závodu"*.

**Zvolená cesta: `CATEGORY-MODE` a `CATEGORY-BAND` se odvozují z reálně
zalogovaných QSO daného závodu**, ne ze zapamatované hodnoty z minulého
exportu. Zapamatovaná hodnota zůstává jen jako fallback pro závod, ve
kterém ještě není ani jedno QSO.

**Proč tahle a ne položky v editoru závodu:** odvozená hodnota je vždy
pravdivá a je automaticky per-závod — přesně to, co tady selhalo. Kdyby
kategorie byla políčkem v nastavení závodu, pořád se spoléhá na to, že ji
operátor vyplní správně (a `exch_def` je při reserializaci zahazuje, takže
by to znamenalo schema v3, tj. migraci živé DB — samostatný krok, který
by chtěl výslovný souhlas). Preset to navíc vědět nemůže: YO DX má
kategorie CW, SSB i MIXED.

**Změny:**
- `src/engine/cabrillo.h` / `cabrillo.c` — nové
  `logfl_cabrillo_categories_from_log()`. Mapování módů na *rodiny*
  (CW→CW, SSB/AM→SSB, FM→FM, RTTY→RTTY, ostatní digi→DIGI), víc rodin
  → `MIXED`; pásmo → Cabrillo designátor, víc pásem → `ALL`.
  Nezmapované pásmo vrátí u pásma `NULL` (fallback) — ale jen dokud je
  log jinak jednopásmový; jakmile se dvě známá pásma liší, je to `ALL`
  tak jako tak.
- `src/app/win.c` — exportní dialog plní Band a Mode odvozenou hodnotou
  a řádky dostanou podtitulek „From the logged QSOs". **Zůstávají
  editovatelné** — log dokazuje, co bylo odpracováno, ne v jaké kategorii
  se startuje (jednopásmový log může legitimně jít do all-band).
- `src/cabrillo_test.c` — nový případ `/cabrillo/categories-from-log`:
  prázdný závod → NULL/NULL, jedno pásmo + jeden mód → `CW`/`20M`, QSO
  v hlavním logu kategorie neovlivní, druhé pásmo a fone → `MIXED`/`ALL`,
  FT8+PSK31 → jedno `DIGI`, nezmapované pásmo → `NULL`.

**Ověřeno:**
- `meson test -C builddir` — **10/10 OK**, včetně nového případu.
- Proti **reálným datům závodu** (kopie `log.db`, contest_ref=4, 107 QSO):
  funkce vrací `CATEGORY-MODE=CW`, `CATEGORY-BAND=20M` — tedy přesně to,
  co v odeslané hlavičce mělo stát místo `RTTY`.

**Neověřeno:** samotný dialog nebyl spuštěn živě — Richardova instance
appky v té chvíli běžela nad ostrou `log.db` a druhý zapisovatel nad týmž
WAL se nespouští. Ověření dialogu (Band/Mode předvyplněné z logu,
podtitulek, ruční přepsání drží) zbývá po restartu appky.

**Nedotčeno:** `~/.config/log-for-linux/settings.ini` (`mode=RTTY` tam
zůstává, ale je nově inertní — odvozená hodnota ho přebíjí) a odeslaný
soubor. Necommitnuto.
