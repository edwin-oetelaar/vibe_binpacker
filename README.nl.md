# vibe_binpacker — 2D nesting voor plaatmateriaal

*[This page in English](README.md)*

Geef het een verzameling vormen en een rechthoekige plaat, en het bepaalt waar
elk onderdeel gesneden moet worden zodat er zo min mogelijk materiaal verloren
gaat. Willekeurige enkelvoudige polygonen, gaten, kwartslagen, en een
gegarandeerde tussenruimte tussen onderdelen. De solver is deterministische
C11 zonder allocatie in het hete pad; Python doet de tooling en een apart
programma is de viewer.

![de viewer](docs/gui_screenshot.png)

Vrij te gebruiken en van te leren. Het is tegelijk een uitgewerkt voorbeeld
van hoe ik optimalisatievraagstukken aanpak, en dat is de andere reden dat het
hier staat — zie [wanneer een goed algoritme niet het
antwoord is](#wanneer-een-goed-algoritme-niet-het-antwoord-is).

**Inhoud** — [Snel starten](#snel-starten) · [Hoe het werkt](#hoe-het-werkt) ·
[Jobformaat](#het-jobformaat) · [Viewer](#de-viewer) · [Bouwen](#bouwen) ·
[Wat de metingen veranderden](#wat-de-metingen-veranderden) ·
[Wanneer een algoritme niet het antwoord is](#wanneer-een-goed-algoritme-niet-het-antwoord-is) ·
[Voor wie dit ertoe doet](#voor-wie-dit-ertoe-doet) ·
[Samenwerken](#samenwerken)

---

## Snel starten

De enige vereiste is [Pixi](https://pixi.sh); de compiler, Python en de
GUI-bibliotheken komen allemaal uit de omgeving die Pixi aanmaakt. Voorlopig
alleen Linux.

```sh
pixi run demo
```

Dat pakt de voorbeeldjobs op een plaat van 1200 × 1200 mm met 1 mm tussen de
onderdelen, en schrijft van elk een SVG naar `build/`:

- [jobs/demo.job](jobs/demo.job) — 36 rechthoeken.
- [jobs/shapes.job](jobs/shapes.job) — L-vormen, driehoeken, een vierkant frame
  en een vijfhoek, op kwartslagen.
- [jobs/nested_hole.job](jobs/nested_hole.job) — een frame en een vierkant dat
  alleen in het gat van dat frame past.
- [jobs/dense.job](jobs/dense.job) — meer onderdelen dan erop passen, gepakt
  met de zoeklaag in plaats van één doorloop.

Elk resultaat wordt daarna nagerekend door
[tools/check_layout.py](tools/check_layout.py), dat de SVG terugleest en
**onafhankelijk van de solver** controleert dat niets overlapt, niets van de
plaat loopt en de tussenruimte klopt. Die controle is geen versiering: hij
ving de fout waarbij de tussenruimte tussen celmiddelpunten werd gemeten in
plaats van tussen celvlakken. Geen enkele unittest van mij had dat gevonden,
want die had ik tegen hetzelfde verkeerde idee geschreven.

Zoeken in plaats van één doorloop, met een tijdsbudget:

```sh
./build/bp_pack --time 10 --svg out.svg jobs/dense.job
```

Op `jobs/demo.job` brengt vijf seconden de gebruikte hoogte van 1134 mm terug
naar 1035 mm — 99 mm méér plaat die als één schone, herbruikbare strook
overblijft.

## Hoe het werkt

De plaat is een raster van bits, celgrootte instelbaar en standaard 1 mm. Bij
1 mm is een plaat van 1200 × 1200 negentien `uint64_t` woorden per rij en
182 KB in totaal, zodat het botsingsorakel in de L2-cache past. Onderdelen
worden conservatief gerasterd tot bitmaskers, opgeslagen als rij-spans, één
masker per toegestane rotatie. Holle vormen en polygonen met gaten hebben in
die representatie geen enkele extra code nodig, en een onderdeel dat *in het
gat van een ander onderdeel* genest wordt volgt er rechtstreeks uit. De prijs
is een kwantisering van 1 mm, en die ligt onder de snijvoeg van de laser.

De tussenruimte gaat zoals de literatuur het doet: elk onderdeel draagt twee
maskers, de echte geometrie en een kopie die met de gevraagde afstand is
opgeblazen. De zoekactie toetst de opgeblazen versie, de plaat registreert de
echte, en dat levert precies de afstand op die je vroeg in plaats van het
dubbele. Het opblazen wordt gemeten tussen cel*vlakken* en niet tussen
celmiddelpunten, want twee cellen die diagonaal één stap uit elkaar liggen
hebben middelpunten op √2 afstand maar vlakken die elkaar in een hoekpunt
raken.

De vraag waar een onderdeel past heeft twee heel verschillende antwoorden. Voor
de *eerste* vrije plek wint een scan die de stukken overslaat die een blokkade
uitsluit, omdat hij een kandidaat meestal al op diens eerste 64-bits woord
afserveert. Voor **elke** vrije plek op een hoogte wordt het antwoord berekend
in plaats van gezocht: het doorsnijden van één run-masker per span levert de
hele rij geldige posities in een handvol woordoperaties, elf tot dertig keer
sneller dan diezelfde verzameling bij elkaar scannen. Dat tweede antwoord kan
een continue methode helemaal niet geven, en het is precies wat een
scorende laag nodig heeft.

Boven op de bottom-left-fill-constructie draaien twee zoeklagen:

- **Ruin & recreate** — maak één gebied van de plaat leeg, zet die onderdelen
  terug samen met alles wat nog niet geplaatst was, en houd het resultaat als
  het beter is.
- **Overlap-minimalisatie** — pers de layout in een smallere band, waardoor
  onderdelen moeten overlappen, en loop die overlap met een guided local search
  terug naar nul.

Ze winnen op verschillende instanties. Herschikken wint waar onderdelen in een
betere stapeling te ordenen zijn; knijpen wint waar de volgorde al goed is en
de layout alleen nog moet inklinken. Geen van beide domineert, en dat is op
zichzelf een resultaat dat het weten waard is.

[PLAN.md](PLAN.md) bevat het volledige ontwerp en de gefaseerde roadmap.
[RESEARCH.md](RESEARCH.md) bevat het literatuuronderzoek dat eraan ten
grondslag ligt, inclusief waarom dit project de rasterroute neemt waar de
hoofdstroom no-fit polygons gebruikt.

## Het jobformaat

Bewust regelgebaseerd, zodat de solver geen parser-afhankelijkheid nodig
heeft. DXF- en SVG-import hoort in tooling die deze bestanden schrijft.

    sheet 1200 1200        # plaatmaat in mm
    cell 1.0               # rasterresolutie in mm
    clearance 1.0          # tussenruimte tussen onderdelen, in mm

    rect 1 2 600 300 1     # id, aantal, breedte, hoogte, mag kwartslag draaien

    poly 2 4 every:90      # id, aantal, hoeken: een lijst als 0,90,180 of
                           # every:<stap> voor een hele slag in gelijke stappen
    ring 6  0 0  300 0  300 80  120 80  120 200  0 200
    ring 4  40 40  80 40  80 80  40 80     # verdere ringen zijn gaten

Een `rect`-regel is suiker voor een polygoon met vier hoekpunten, zodat er
maar één geometriepad is. Windingsrichting doet niet mee: de vulling gebruikt
de even-odd-regel, omdat echte CAD-export willekeurig gewonden binnenkomt en
ernaar gokken erger zou zijn dan hem negeren.

## De viewer

Een pakking is iets ruimtelijks, en een kolom getallen verbergt alles wat
eraan telt: welke hoek misging, welk onderdeel gestrand is, of het afval één
bruikbare strook is of verspreide snippers.

```sh
pixi run fetch-gui-deps
pixi run gui
./build/bp_gui jobs/shapes.job
```

Slepen om te pannen, wiel om te zoomen, en de step-slider speelt de
plaatsingsvolgorde terug — de snelste manier om een heuristiek een slechte
beslissing te zien nemen. De knoppen herpakken zonder herstarten.

| | |
| --- | --- |
| `Ctrl+O` | job openen, met een browsertje dat `.job`-bestanden toont |
| `Ctrl+R` | herladen, pakt een gewijzigd bestand of nieuwe celgrootte op |
| `F5` | opnieuw pakken |
| `Ctrl+S` | layout als SVG wegschrijven |
| `Ctrl+Q` | afsluiten |

Afsluiten met een niet-opgeslagen pakking vraagt eerst, of je nu het menu, de
sneltoets of de vensterknop gebruikt.

De viewer is het enige onderdeel van dit project met een externe
afhankelijkheid, en staat bewust apart: `pixi run test`, `demo` en `benchmark`
raken hem niet, en [tools/fetch_gui_deps.sh](tools/fetch_gui_deps.sh) pint de
revisie in plaats van een kopie van Dear ImGui mee te committen. Het venster
zelf is puur C — cimgui is de gegenereerde C-API voor Dear ImGui, en het C++
eronder wordt apart gecompileerd. Onderdelen gaan rechtstreeks de ImGui
draw-list in als samengevoegde rechthoeken, dus er zijn geen shaders en geen
textures.

`./build/bp_gui <job> --shot frame.ppm --frames 12` rendert een paar frames,
schrijft de framebuffer weg en stopt. Zo controleer je een venster vanuit een
script. Een GUI die bouwt is geen GUI die tekent.

### Als het venster wel in de taakbalk staat maar leeg blijft

GLFW kiest Wayland zodra `WAYLAND_DISPLAY` gezet is; onder WSLg vindt de EGL
daar geen driver, valt terug op zink, vindt geen device en presenteert niets.
De viewer vraagt daarom standaard om **X11**, wat zowel onder WSLg als op een
echte Wayland-desktop via XWayland werkt. `--platform wayland` en
`--platform auto` overrulen dat.

Verschijnt er helemaal niets, dan print `./build/bp_gui --diagnose` de
backend, de GL-driver, waar het venster denkt te staan en hoeveel frames er
werkelijk geproduceerd zijn, en stopt dan. Ziet dat er allemaal gezond uit en
blijft het scherm leeg, dan zit het displaypad zelf vast — onder WSL herstart
`wsl --shutdown` vanuit PowerShell het geheel.

## Bouwen

```sh
pixi run test
```

bouwt de solver en draait alle tests. De eerste keer maakt hij ook de omgeving
aan, wat een minuut kost.

| taak | wat het doet |
| ---- | ------------ |
| `pixi run build` | compileert de programma's en de tests |
| `pixi run test` | bouwt en draait alle tests |
| `pixi run test-sanitize` | hetzelfde onder AddressSanitizer en UndefinedBehaviorSanitizer |
| `pixi run demo` | pakt de voorbeeldjobs en controleert elk resultaat na |
| `pixi run benchmark` | doorloopt celgroottes en zoekmodi, print CSV |
| `pixi run fetch-gui-deps` | haalt de gepinde afhankelijkheden van de viewer |
| `pixi run gui` | bouwt de viewer |
| `pixi run clean` | `rm -rf build` |

Alles wat de build oplevert komt onder `build/` terecht en niets anders daar,
zodat opruimen het verwijderen van één map is. Kale `make` werkt ook als je
liever je eigen toolchain meebrengt: de Makefile gaat van niets meer uit dan
een C11-compiler, en `make gui` heeft daarnaast GLFW en de OpenGL-headers
nodig.

Compilervlaggen liggen vast op `-std=c11 -Wall -Wextra -Werror -pedantic`, en
sanitizer-runs zijn verplicht bij elke wijziging aan indexering of
bitverschuiving. Tests worden via wildcard gevonden, dus `tests/test_iets.c`
toevoegen is genoeg om hem te laten bouwen en draaien.

    include/   elke publieke header, één per module
    src/       de solver, één implementatiebestand per header
    apps/      bp_pack (de CLI) en bp_bench (de benchmark)
    tests/     één testprogramma per module
    gui/       de viewer
    jobs/      voorbeeldjobs
    tools/     Python-tooling
    build/     alles wat de build oplevert

## Wat de metingen veranderden

Dit is het stuk waar ik werkelijk naar zou wijzen, want het is het verschil
tussen software die er goed uitziet en software die goed ís. Elk van deze
punten wierp de voor de hand liggende aanpak omver, en elk kwam alleen boven
water doordat er gemeten werd. Ze staan allemaal met cijfers uitgewerkt in
[PLAN.md](PLAN.md).

- **De slimme sprong was de verkeerde ruil.** Zo ver vooruitspringen als al het
  bewijs toelaat scheelde 0,4 % kandidaatposities en kostte zeventig keer meer
  werk per positie. Stoppen bij de eerste blokkade is 69 keer sneller.
- **De "echte motor" verloor.** Het plan noemde morfologische erosie de kern
  van de methode. Gemeten is hij *trager* dan een gewone scan voor het vinden
  van de eerste vrije plek — en elf tot dertig keer sneller voor het opsommen
  van álle vrije plekken, wat een andere vraag is en degene die verderop telt.
- **Tussenruimte gemeten tussen celmiddelpunten laat onderdelen elkaar in de
  hoeken raken.** Twee cellen diagonaal één stap uit elkaar liggen op √2
  afstand van middelpunt tot middelpunt en op nul afstand van materiaal tot
  materiaal. Gevonden door de onafhankelijke controle, niet door een unittest.
- **Een greedy herstellus is een vast punt.** Ruin & recreate leverde precies
  nul verbeteringen op over vijfduizend rondes, totdat de ruin een aaneengesloten
  gebied leegmaakte en het herstel een paar kandidaten willekeurig mocht
  overslaan.
- **Een straf die alleen de keuze stuurt is geen guided local search.** Met
  straffen die bepaalden wélk onderdeel beweegt maar niet wat een positie
  kost, loste geen enkele knijpstap op een volle plaat ooit op. Het gewicht in
  de kósten zetten liet ze allemaal oplossen, in 15 tot 114 iteraties in plaats
  van vastlopen bij duizend.
- **Contactscore, de klassieke heuristiek, verdiende zijn standaardwaarde
  niet.** Elke positieve weging plaatste vijf onderdelen mínder dan gewoon
  bottom-left op een overvraagde plaat. Hij is geïmplementeerd, getest,
  beschikbaar als vlag, en staat standaard op nul.

Negatieve resultaten zijn er met opzet in blijven staan. Een repository die
alleen vastlegt wat werkte leert je bijzonder weinig over hoe je er komt.

## Wanneer een goed algoritme niet het antwoord is

Een nesting-algoritme optimaliseert één ding: de schikking van onderdelen op
één plaat. Dat is zelden waar het grootste deel van je materiaalkosten wordt
bepaald.

Wat het wél bepaalt: welke orders samen gesneden worden, welke plaatmaten je
inkoopt, of restmateriaal met een bruikbare geometrie terugkomt in voorraad of
in de container verdwijnt, walsrichting en afwerking, snijvolgorde en
warmte-inbreng, common-line cutting, klemzones en kantelrisico bij kleine
delen, materiaalcharges en traceerbaarheid, en — altijd — wat je operators
werkelijk doen als het plan onhandig uitkomt. Een solver die dat negeert kan
aantoonbaar optimaal zijn en toch geld kosten.

En dan moet het nog landen. Waarde ontstaat als het draait binnen de stroom
die je al hebt: je ERP of MES, je CAM-post, je DXF-conventies, je
onderdelenbibliotheek, je naamgeving, zonder dat iemand iets overtypt. Dat
deel is niet spannend en het is het meeste werk.

Ik ben hier wantrouwig tegenover de kortste route naar een antwoord. Een
taalmodel produceert binnen een minuut een plausibel plan, en het klinkt goed.
Wat het niet doet, is meten of dat plan waar is voor *jouw* materiaal, *jouw*
machine en *jouw* orderboek. Zes van de bevindingen hierboven spreken de voor
de hand liggende aanpak tegen; geen ervan had een zelfverzekerde samenvatting
overleefd, en elk was echt materiaal waard. Ik breng liever een getal dan een
mening.

Dus: gebruik deze repository gerust. Is wat je nodig hebt een meetbare
vermindering van afval binnen een echt productieproces, dan is dat een ander
werk, en dat is het werk dat ik doe.

## Voor wie dit ertoe doet

Overal waar onderdelen uit voorraadmateriaal worden gesneden en de restsnippers
geld zijn:

| | |
| --- | --- |
| **Plaatbewerking** | laser-, plasma- en waterstraalsnijderijen; machinebouw en toeleveranciers |
| **Staalbouw en scheepsbouw** | plaatnesting op grote formaten, waar een procent al snel tonnen is |
| **Meubel- en interieurbouw** | CNC-nesting in MDF, multiplex en spaanplaat, met nerfrichting |
| **Glas, kunststof, sandwichpaneel** | acrylaat, polycarbonaat, dibond, honingraat |
| **Textiel, leer, stoffering** | confectie, meubelbekleding, auto-interieur, met patroon- en vleugrichting |
| **Verpakking en stansen** | karton- en golfkartonlayouts, stansvormen |
| **Composieten** | prepreg en droog weefsel per kit snijden, waar materiaal de kostenpost is |
| **Isolatie, rubber, pakkingen** | seriematig snijden uit rollen en platen |
| **Natuursteen, keramiek, werkbladen** | slabs met natuurlijke gebreken en aderaansluiting — echt lastige nesting |
| **Signing en printafwerking** | foamboard, vinyl, dibond, veel korte series |
| **Elektronica** | paneelindeling en depanelisatie van printplaten |

Blijkt de knelpunt helemaal niet de plaat te zijn, dan dekt dezelfde
gereedschapskist ook productieplanning, ordergroepering en batching, capaciteits-
en ploegenplanning, routering en scheduling. Operations research is het
vakgebied; nesting is er één vraagstuk uit.

## Samenwerken

Ik ben **Edwin van den Oetelaar**, zelfstandig softwareontwikkelaar in
Nederland. Ik bouw optimalisatiesoftware voor bedrijven die snijden, plannen
en inroosteren — van het algoritme tot en met de koppeling met de systemen die
je al draait.

Ik ben afgestudeerd in technische bedrijfskunde en operations research, en
daarom vind ik dit soort puzzels na al die jaren nog steeds leuk, en daarom
meet ik liever dan dat ik aanneem. Deze repository laat zien hoe dat er in de
praktijk uitziet: benchmarks vóór beweringen, negatieve resultaten bewaard, en
een controleprogramma dat de solver niet gelooft.

<!-- CONTACTGEGEVENS: hier invullen voor publicatie -->

> ### Neem contact op
>
> **_Dit blok moet nog ingevuld worden._**
>
> - **Edwin van den Oetelaar** — zelfstandig softwareontwikkelaar, Nederland
> - E-mail: _nog toevoegen_
> - LinkedIn: _nog toevoegen_
> - Website: _nog toevoegen_
> - Telefoon: _nog toevoegen_
>
> Ik praat graag een concrete casus door: wat je snijdt, waaruit, in welke
> aantallen, en waar het afval nu zit.

## Licentie

<!-- LICENTIE: kies er een en voeg een LICENSE-bestand toe voor publicatie -->

_Nog niet gekozen._ Zolang er geen `LICENSE`-bestand is geldt het standaard
auteursrecht en wordt er geen toestemming tot hergebruik verleend. Wil je dit
gebruiken, neem dan contact op; dat is zo geregeld.

## Conventies

De C-stijl volgt Pieter Hintjens' Scalable C: één header en één
implementatiebestand per module, `bp_` voor publieke symbolen en `s_` voor
file-local statics, caller-owned buffers, geen globale mutable state,
eenheden in namen (`width_cells`, `kerf_mm`). Ongeldige invoer van de
aanroeper is een programmeerfout en faalt direct; verwachte runtime-fouten
geven expliciete getagde resultaten terug. Zie [AGENTS.md](AGENTS.md).
