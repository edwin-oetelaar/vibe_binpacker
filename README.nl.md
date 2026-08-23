# vibe_binpacker — 2D nesting voor plaatmateriaal

*[This page in English](README.md)*

Je geeft het een set onderdelen en een rechthoekige plaat, en het bepaalt waar
elk onderdeel gesneden moet worden zodat er zo min mogelijk materiaal verloren
gaat. Willekeurige polygonen, gaten, kwartslagen, en een gegarandeerde
tussenruimte tussen de onderdelen. De solver is deterministische C11 die
tijdens het rekenen niets alloceert; het gereedschap eromheen is Python en de
viewer is een apart programma.

![de viewer](docs/gui_screenshot.png)

Vrij te gebruiken en van te leren. Het is tegelijk een uitgewerkt voorbeeld van
hoe ik optimalisatievraagstukken aanpak, en dat is de tweede reden dat het hier
staat — zie [wanneer een goed algoritme niet
genoeg is](#wanneer-een-goed-algoritme-niet-genoeg-is).

**Inhoud** — [Snel starten](#snel-starten) · [Hoe het werkt](#hoe-het-werkt) ·
[Jobformaat](#het-jobformaat) · [Viewer](#de-viewer) · [Bouwen](#bouwen) ·
[Wat de metingen veranderden](#wat-de-metingen-veranderden) ·
[Wanneer een algoritme niet genoeg is](#wanneer-een-goed-algoritme-niet-genoeg-is) ·
[Voor wie dit interessant is](#voor-wie-dit-interessant-is) ·
[Samenwerken](#samenwerken)

---

## Snel starten

Het enige dat je nodig hebt is [Pixi](https://pixi.sh). De compiler, Python en
de GUI-bibliotheken komen allemaal uit de omgeving die Pixi voor je aanmaakt.
Voorlopig alleen Linux.

```sh
pixi run demo
```

Dat nest de voorbeeldjobs op een plaat van 1200 × 1200 mm met 1 mm tussen de
onderdelen, en schrijft van elk een SVG naar `build/`:

- [jobs/demo.job](jobs/demo.job) — 36 rechthoeken.
- [jobs/shapes.job](jobs/shapes.job) — L-vormen, driehoeken, een vierkant frame
  en een vijfhoek, op kwartslagen.
- [jobs/nested_hole.job](jobs/nested_hole.job) — een frame en een vierkant dat
  alleen in het gat van dat frame past.
- [jobs/dense.job](jobs/dense.job) — meer onderdelen dan er passen, genest met
  de zoeklaag in plaats van in één doorloop.

Elk resultaat gaat daarna langs
[tools/check_layout.py](tools/check_layout.py). Dat leest de SVG terug en
controleert **buiten de solver om** dat niets overlapt, niets buiten de plaat
valt en de tussenruimte klopt. Die controle is geen franje: hij ving de fout
waarbij de tussenruimte tussen celmiddelpunten werd gemeten in plaats van
tussen de cellen zelf. Geen van mijn unittests had dat gevonden, want die had
ik op hetzelfde verkeerde idee gebaseerd.

Wil je zoeken in plaats van één doorloop, geef het dan een tijdsbudget:

```sh
./build/bp_pack --time 10 --svg out.svg jobs/dense.job
```

Bij `jobs/demo.job` brengt vijf seconden de benutte hoogte terug van 1134 mm
naar 1035 mm. Dat is 99 mm extra plaat die als één rechte, herbruikbare strook
overblijft.

## Hoe het werkt

De plaat is een raster van bits, met een instelbare celgrootte van standaard
1 mm. Bij 1 mm beslaat een plaat van 1200 × 1200 negentien `uint64_t` woorden
per rij en 182 KB in totaal, klein genoeg om in de L2-cache te blijven staan.
Onderdelen worden zo gerasterd dat elke cel die het onderdeel ook maar
aanraakt meetelt, en het resultaat wordt opgeslagen als rijen aaneengesloten
stukken: één masker per toegestane rotatie. Holle vormen en polygonen met
gaten hebben in die vorm geen enkele extra code nodig, en dat een onderdeel *in
het gat van een ander onderdeel* terechtkomt volgt er rechtstreeks uit. De prijs
is een rastering van 1 mm, en die valt binnen de snijvoeg van de laser.

De tussenruimte werkt zoals de vakliteratuur het voorschrijft. Elk onderdeel
heeft twee maskers: de echte contour, en een kopie die met de gevraagde afstand
is opgeblazen. De zoekactie toetst de opgeblazen versie, terwijl de plaat de
echte contour vasthoudt. Zo houd je precies de afstand over die je vroeg, in
plaats van het dubbele. Het opblazen wordt gemeten tussen de cellen zelf en
niet tussen hun middelpunten, want twee cellen die diagonaal naast elkaar
liggen hebben middelpunten op √2 afstand, maar raken elkaar in een hoekpunt.

De vraag "waar past dit onderdeel" heeft twee heel verschillende antwoorden.
Zoek je de *eerste* vrije plek, dan wint een scan die de stukken overslaat die
een blokkade sowieso uitsluit: die wijst een kandidaat meestal al op het eerste
64-bits woord af. Zoek je **alle** vrije plekken op een bepaalde hoogte, dan
wordt het antwoord berekend in plaats van gezocht. Door per aaneengesloten stuk
één masker te maken en die te doorsnijden krijg je de hele rij geldige posities
in een handvol woordbewerkingen, elf tot dertig keer sneller dan diezelfde
verzameling bij elkaar scannen. Dat tweede antwoord kan een methode die met
continue coördinaten werkt helemaal niet geven, en het is precies wat een
scorende laag nodig heeft.

Boven op de bottom-left-fill-constructie draaien twee zoeklagen:

- **Ruin & recreate** — maak één gebied van de plaat leeg, leg die onderdelen
  opnieuw samen met alles wat nog niet geplaatst was, en houd het resultaat als
  het beter is.
- **Overlap-minimalisatie** — pers het legplan in een smallere band, waardoor
  onderdelen elkaar moeten overlappen, en werk die overlap met een guided local
  search terug naar nul.

Ze winnen elk op andere gevallen. Herschikken wint als de onderdelen in een
betere volgorde gelegd kunnen worden; persen wint als de volgorde al klopt en
het legplan alleen nog moet inklinken. Geen van beide is altijd de beste, en
dat is op zichzelf al iets waard om te weten.

[PLAN.md](PLAN.md) bevat het volledige ontwerp en de roadmap.
[RESEARCH.md](RESEARCH.md) bevat het literatuuronderzoek waar het op rust, met
daarin ook waarom dit project de rasterroute kiest waar de meeste publicaties
no-fit polygons gebruiken.

## Het jobformaat

Bewust regel voor regel opgebouwd, zodat de solver geen parser van derden nodig
heeft. Import vanuit DXF of SVG hoort thuis in gereedschap dat deze bestanden
schrijft.

    sheet 1200 1200        # plaatmaat in mm
    cell 1.0               # rasterresolutie in mm
    clearance 1.0          # tussenruimte tussen onderdelen, in mm

    rect 1 2 600 300 1     # id, aantal, breedte, hoogte, mag kwartslag draaien

    poly 2 4 every:90      # id, aantal, hoeken: een lijst als 0,90,180 of
                           # every:<stap> voor een hele slag in gelijke stappen
    ring 6  0 0  300 0  300 80  120 80  120 200  0 200
    ring 4  40 40  80 40  80 80  40 80     # volgende ringen zijn gaten

Een `rect`-regel is niet meer dan een korte schrijfwijze voor een polygoon met
vier hoekpunten. Zo is er maar één manier waarop geometrie het programma
binnenkomt, en dus ook maar één plek waar het fout kan gaan. De draairichting
van een ring maakt niet uit: het vullen gaat volgens de even-oddregel, omdat
CAD-export in de praktijk met willekeurige draairichting binnenkomt en ernaar
raden slechter is dan hem negeren.

## De viewer

Een legplan is iets ruimtelijks, en een kolom cijfers verbergt juist datgene
wat eraan telt: welke hoek misging, welk onderdeel is blijven liggen, en of het
afval één bruikbare strook is of allemaal losse snippers.

```sh
pixi run fetch-gui-deps
pixi run gui
./build/bp_gui jobs/shapes.job
```

Slepen om te schuiven, wiel om te zoomen, en met de stapschuif speel je de
volgorde van plaatsen terug. Dat is de snelste manier om een heuristiek een
verkeerde beslissing te zien nemen. De instellingen nesten opnieuw zonder dat
je het programma hoeft te herstarten.

| | |
| --- | --- |
| `Ctrl+O` | job openen, met een klein venster dat `.job`-bestanden toont |
| `Ctrl+R` | opnieuw inlezen, zodat een aangepast bestand of celgrootte meetelt |
| `F5` | opnieuw nesten |
| `Ctrl+S` | legplan als SVG wegschrijven |
| `Ctrl+Q` | afsluiten |

Afsluiten met een legplan dat je nog niet bewaard hebt vraagt eerst om
bevestiging, of je nu het menu, de sneltoets of de vensterknop gebruikt.

De viewer is het enige deel van dit project dat een bibliotheek van derden
gebruikt, en hij staat daarom apart: `pixi run test`, `demo` en `benchmark`
raken hem niet aan, en [tools/fetch_gui_deps.sh](tools/fetch_gui_deps.sh) legt
de revisie vast in plaats van een kopie van Dear ImGui mee te leveren. Het
venster zelf is gewoon C — cimgui is de gegenereerde C-interface op Dear ImGui,
en de C++ die eronder zit wordt apart gecompileerd. Onderdelen gaan als
samengevoegde rechthoeken rechtstreeks de tekenlijst van ImGui in, dus er komt
geen enkele shader of texture aan te pas.

Met `./build/bp_gui <job> --shot frame.ppm --frames 12` render je een paar
frames, schrijf je de framebuffer weg en stopt het programma. Zo controleer je
een venster vanuit een script. Een GUI die compileert is nog geen GUI die
tekent.

### Als het venster wel in de taakbalk staat maar leeg blijft

GLFW kiest Wayland zodra `WAYLAND_DISPLAY` gezet is. Onder WSLg vindt de EGL
daar geen driver, valt hij terug op zink, vindt daar geen apparaat, en komt er
niets op je scherm. De viewer vraagt daarom standaard om **X11**, wat zowel
onder WSLg als op een echte Wayland-desktop via XWayland werkt. Met
`--platform wayland` of `--platform auto` kies je zelf.

Zie je helemaal niets, dan drukt `./build/bp_gui --diagnose` af welke backend
en welke GL-driver actief zijn, waar het venster denkt te staan en hoeveel
frames er werkelijk getekend zijn, en stopt daarna. Ziet dat er allemaal gezond
uit en blijft je scherm toch leeg, dan zit het beeldpad zelf vast. Onder WSL
zet `wsl --shutdown` vanuit PowerShell alles opnieuw op.

## Bouwen

```sh
pixi run test
```

bouwt de solver en draait alle tests. De eerste keer maakt Pixi ook de omgeving
aan; dat duurt ongeveer een minuut.

| taak | wat het doet |
| ---- | ------------ |
| `pixi run build` | compileert de programma's en de tests |
| `pixi run test` | bouwt en draait alle tests |
| `pixi run test-sanitize` | hetzelfde, maar onder AddressSanitizer en UndefinedBehaviorSanitizer |
| `pixi run demo` | nest de voorbeeldjobs en controleert elk resultaat na |
| `pixi run benchmark` | loopt celgroottes en zoekmodi af en drukt CSV af |
| `pixi run fetch-gui-deps` | haalt de vastgelegde bibliotheken voor de viewer op |
| `pixi run gui` | bouwt de viewer |
| `pixi run clean` | `rm -rf build` |

Alles wat de build oplevert komt in `build/` terecht, en daar staat verder
niets anders. Opruimen is dus het weggooien van één map. Wil je liever je eigen
toolchain gebruiken, dan werkt gewoon `make` ook: de Makefile veronderstelt
niets meer dan een C11-compiler, en `make gui` heeft daarnaast GLFW en de
OpenGL-headers nodig.

De compilervlaggen liggen vast op `-std=c11 -Wall -Wextra -Werror -pedantic`,
en bij elke wijziging aan indexering of bitverschuiving hoort een run onder de
sanitizers. Tests worden met een wildcard gevonden, dus een bestand
`tests/test_iets.c` toevoegen is genoeg om het mee te laten bouwen en draaien.

    include/   alle publieke headers, één per module
    src/       de solver, één implementatiebestand per header
    apps/      bp_pack (de opdrachtregel) en bp_bench (de benchmark)
    tests/     één testprogramma per module
    gui/       de viewer
    jobs/      voorbeeldjobs
    tools/     Python-gereedschap
    build/     alles wat de build oplevert

## Wat de metingen veranderden

Dit is het deel waar ik echt naar zou wijzen, want hier zit het verschil tussen
software die er goed uitziet en software die het ook is. Elk van deze punten
zette de voor de hand liggende aanpak op zijn kop, en elk kwam pas boven water
doordat er gemeten werd. Ze staan met cijfers en al uitgewerkt in
[PLAN.md](PLAN.md).

- **De slimme sprong bleek een slechte ruil.** Zo ver vooruitspringen als alle
  aanwijzingen toelaten leverde 0,4 % minder kandidaatposities op en kostte
  zeventig keer zoveel werk per positie. Stoppen bij de eerste blokkade is 69
  keer sneller.
- **De "echte motor" verloor.** Het plan noemde morfologische erosie de kern
  van de methode. Gemeten blijkt hij *trager* dan een gewone scan als je de
  eerste vrije plek zoekt — en elf tot dertig keer sneller als je álle vrije
  plekken wilt opsommen. Dat is een andere vraag, en het is degene die
  verderop in de keten telt.
- **Tussenruimte meten tussen celmiddelpunten laat onderdelen elkaar in de
  hoeken raken.** Twee cellen die diagonaal naast elkaar liggen zitten op √2
  afstand van middelpunt tot middelpunt, maar op nul afstand van materiaal tot
  materiaal. Gevonden door de controle buiten de solver om, niet door een
  unittest.
- **Een gulzige herstellus komt nooit van zijn plek.** Ruin & recreate leverde
  over vijfduizend rondes precies nul verbeteringen op, totdat het opruimen een
  aaneengesloten gebied leeg ging maken en het opnieuw leggen af en toe een
  kandidaat mocht overslaan.
- **Een strafpunt dat alleen de keuze stuurt is geen guided local search.**
  Zolang de strafpunten bepaalden wélk onderdeel verplaatst werd, maar niet wat
  een positie kost, loste geen enkele persstap op een volle plaat ooit op. Het
  gewicht in de kósten verwerken liet ze allemaal slagen, in 15 tot 114
  iteraties in plaats van vastlopen na duizend.
- **De contactscore, de klassieke heuristiek, verdiende zijn plek niet.** Elke
  positieve weging legde vijf onderdelen mínder op een overvolle plaat dan
  gewoon bottom-left. Hij zit erin, is getest, is als optie beschikbaar, en
  staat standaard uit.

De uitkomsten die tegenvielen zijn er met opzet in blijven staan. Een
repository die alleen laat zien wat werkte, vertelt je maar bar weinig over hoe
je er komt.

## Wanneer een goed algoritme niet genoeg is

Een nesting-algoritme optimaliseert één ding: hoe de onderdelen op één plaat
liggen. En daar wordt je materiaalkost meestal niet bepaald.

Wat die wél bepaalt: welke orders je samen laat snijden, welke plaatmaten je
inkoopt, of restmateriaal met een bruikbare vorm terugkomt in het magazijn of
in de container verdwijnt, walsrichting en afwerking, snijvolgorde en
warmte-inbreng, common-line cutting, klemzones en het kantelrisico van kleine
delen, charges en traceerbaarheid — en altijd: wat je operators daadwerkelijk
doen als het plan hun niet uitkomt. Een solver die dat allemaal negeert kan
aantoonbaar optimaal zijn en je toch geld kosten.

En dan moet het nog ergens landen. Waarde ontstaat pas als het meedraait in de
keten die je al hebt: je ERP of MES, je CAM-postprocessor, jouw
DXF-afspraken, jouw onderdelenbibliotheek, jouw naamgeving, zonder dat iemand
iets hoeft over te typen. Dat deel is niet spannend, en het is wel het meeste
werk.

Ik ben wantrouwig tegenover de kortste route naar een antwoord. Een taalmodel
levert binnen een minuut een plan dat aannemelijk klinkt. Wat het niet doet, is
nagaan of dat plan klopt voor *jouw* materiaal, *jouw* machine en *jouw*
orderboek. Zes van de bevindingen hierboven spreken de voor de hand liggende
aanpak tegen. Geen ervan had een vlot geschreven samenvatting overleefd, en
elke bevinding was echt materiaal waard. Ik lever liever een getal dan een
mening.

Gebruik deze repository dus gerust. Maar heb je een meetbare vermindering van
afval nodig binnen een echt productieproces, dan is dat ander werk — en dat is
het werk dat ik doe.

## Voor wie dit interessant is

Overal waar onderdelen uit voorraadmateriaal worden gesneden en het
restmateriaal geld waard is:

| | |
| --- | --- |
| **Plaatbewerking** | laser-, plasma- en waterstraalsnijderijen, machinebouw en hun toeleveranciers |
| **Staalbouw en scheepsbouw** | nesting op grote plaatformaten, waar één procent al snel tonnen scheelt |
| **Meubel- en interieurbouw** | CNC-nesting in MDF, multiplex en spaanplaat, inclusief nerfrichting |
| **Glas, kunststof en sandwichpanelen** | acrylaat, polycarbonaat, dibond, honingraat |
| **Textiel, leer en stoffering** | confectie, meubelbekleding, auto-interieur, met patroon- en vleugrichting |
| **Verpakking en stansen** | indelingen voor karton en golfkarton, stansvormen |
| **Composieten** | prepreg en droog weefsel per kit snijden, waar het materiaal de grootste kostenpost is |
| **Isolatie, rubber en pakkingen** | seriematig snijden uit rollen en platen |
| **Natuursteen, keramiek en werkbladen** | slabs met natuurlijke gebreken en aders die moeten aansluiten: echt lastige nesting |
| **Signing en printafwerking** | foamboard, vinyl, dibond, veel korte series |
| **Elektronica** | paneelindeling en depanelisatie van printplaten |

Blijkt het knelpunt uiteindelijk helemaal niet de plaat te zijn, dan dekt
dezelfde gereedschapskist ook productieplanning, het groeperen van orders,
capaciteits- en ploegenplanning, routering en scheduling. Operations research
is het vakgebied; nesting is er één vraagstuk uit.

## Samenwerken

Ik ben **Edwin van den Oetelaar**, zelfstandig softwareontwikkelaar in
Nederland. Ik bouw optimalisatiesoftware voor bedrijven die snijden, plannen en
inroosteren — van het algoritme tot en met de koppeling met de systemen die je
al draait.

Ik ben afgestudeerd in technische bedrijfskunde en operations research. Daarom
vind ik dit soort puzzels na al die jaren nog steeds leuk, en daarom meet ik
liever dan dat ik iets aanneem. Deze repository laat zien hoe dat er in de
praktijk uitziet: eerst meten en dan pas iets beweren, tegenvallende
uitkomsten laten staan, en een controleprogramma dat de solver niet op zijn
woord gelooft.

Mijn week is verdeeld. Drie dagen sta ik voor de klas bij Fontys ICT, bij
software engineering en informatietechnologie. De andere twee bouw ik zelf, op
het grensvlak van wat er kan, met AI en robots — en het mooiste vind ik het
als dat de praktijk haalt. In Brainport grenst de theorie van informatica en
wiskunde aan een regio die daadwerkelijk dingen maakt, en praktische engineers
zijn wat die twee werelden verbindt. Dat probeer ik studenten mee te geven, en
het is ook precies wat ik voor klanten doe.

Deze repository hoort bij allebei. Een klant kan erin zien hoe ik werk. Een
student kan erin zien dat een probleem meestal meer dan één goed antwoord
heeft, en dat de verkeerde afslagen het deel zijn waar je iets van leert.

### Twee manieren om dit aan te pakken

Heb je een duidelijk afgebakende uitdaging en wil je snel een concreet
resultaat, dan doe ik het zelf. Dat is commercieel werk, in mijn eigen tijd, en
het gaat zoals dit project ging: meten, laten zien wat eruit komt, en pas iets
beweren als er een getal onder ligt.

De andere weg is een onderzoeksproject met studenten, waarbij ik ze inhoudelijk
begeleid. Dat werkt anders, en het is niet meer dan eerlijk om dat vooraf te
zeggen: studenten moeten er in de eerste plaats van leren. Ze hebben tijd nodig
om de basis te verkennen en om jouw domein te leren kennen, en dat kost maanden
waar ik weken zou doen. Daar staat iets tegenover wat je met inhuren niet
krijgt: je ziet jonge engineers een half jaar lang werken aan jouw echte
probleem, en dat is een van de betere manieren om nieuwe mensen te scouten.

Welke route past, hangt af van wat je nodig hebt. Zit er haast op, of is het
resultaat kritisch, dan is de eerste de verstandige. Heb je ruimte en zoek je
ook aanwas, dan is de tweede het overwegen waard.

> ### Neem contact op
>
> **Edwin van den Oetelaar** — zelfstandig softwareontwikkelaar, Nederland
>
> - E-mail: **[edwin@oetelaar.com](mailto:edwin@oetelaar.com)**
> - Telefoon: **[06 5332 9077](tel:+31653329077)** (+31 6 5332 9077)
> - Web: **[oetelaar.com](https://www.oetelaar.com/)**
> - LinkedIn: **[linkedin.com/in/oetelaar](https://www.linkedin.com/in/oetelaar/)**
>
> Ik praat graag een concrete casus door: wat je snijdt, waaruit, in welke
> aantallen, en waar het afval nu zit.

## Licentie

[MIT](LICENSE) — gebruiken, aanpassen en uitleveren mag, ook commercieel.
Houd de copyrightvermelding erbij en verder ga je gang.

## Conventies

De C-stijl volgt Pieter Hintjens' Scalable C: één header en één
implementatiebestand per module, `bp_` voor publieke symbolen en `s_` voor
functies die alleen binnen een bestand leven, buffers die de aanroeper zelf
beheert, geen globale veranderlijke toestand, en eenheden in namen
(`width_cells`, `kerf_mm`). Verkeerde invoer van de aanroeper is een
programmeerfout en breekt meteen af; fouten die je in bedrijf mag verwachten
komen terug als een expliciet resultaat. Zie [AGENTS.md](AGENTS.md).
