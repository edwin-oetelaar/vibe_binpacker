# Plan: plaatformaten kiezen in de GUI en CLI

Status: geimplementeerd. A en B: `bp_sheet` en `bp_job_load_with`, getest in
`tests/test_bp_sheet.c` en `tests/test_bp_job.c`. C: presets, eigen maat,
Wissel B/H, voorvertoning en Apply & Pack in de GUI, `--sheet-size` in
bp_pack en bp_gui. D: `tools/check_gui_sheet_size.py` en
`tools/measure_sheet_sizes.py`, met de resultaten in de README's en hieronder.
De bestaande `bp_job_load(path, cell)` houdt bewust zijn oude gedrag zonder
productlimieten, zodat de resolutiesweep van bp_bench blijft werken.

## Uitgangspunt

De solver gebruikt de `sheet <breedte_mm> <hoogte_mm>`-regel uit de job.
1200 x 1200 mm is een voorbeeldmaat, geen vaste maat in de collision engine.
Een job met `sheet 800 600` kan dus nu al worden verwerkt.

De uitbreiding maakt deze keuze zichtbaar en eenvoudig in de GUI, met dezelfde
overrides in de CLI. De gekozen scope is vrij plaatformaat: 1250 x 2500 mm mag
als nestformaat worden gebruikt. Voor een laser met een werkgebied van
800 x 600 mm kies je een passend te verwerken stuk. Het voorzagen van een grote
voorraadplaat is een afzonderlijke bewerking.

Een run heeft een plaatformaat; in multi-plaatmodus gebruiken alle platen in
die run dat formaat. Een rechthoekig reststuk kan als eigen maat worden
ingevoerd. Onregelmatige reststukken en een voorraad met verschillende maten
per plaat vragen een volgende uitbreiding van het containermodel.

## 1. Bediening

Voeg onder Geometry het volgende toe:

```text
Plaatformaat: [Uit jobbestand                 v]
               500 x 500 mm
               600 x 600 mm
               600 x 300 mm
               800 x 600 mm - laser
              1200 x 1200 mm
              1250 x 2500 mm
               Eigen maat

Breedte: [800.0] mm    Hoogte: [600.0] mm    [Wissel B/H]
Raster:  [1.0] mm
Rasterplaat: 800 x 600 cellen
Plaatbuffers: ongeveer 5.56 MiB
[Toepassen en nesten]
```

- Begin met Uit jobbestand, zodat bestaande jobs hun eigen geometrie houden.
- Een preset vult de invoervelden. Handmatig aanpassen kiest Eigen maat.
- Wissel B/H is een expliciete actie; afmetingen worden niet stil gesorteerd.
- Toon direct het aantal rastercellen en de geraamde plaatbuffers.
- De bestaande Pack-actie past geldige formaat-/rasterwijzigingen toe voordat
  hij opnieuw nest. Bij pending wijzigingen heet de knop Apply & Pack.
- Tot dat moment blijft de oude indeling met haar eigen maat, schaal,
  statistieken en SVG-export geldig. Toon duidelijk dat de wijziging nog niet
  is toegepast.
- Lees bij laden standaard ook de celgrootte uit de job; een expliciete
  rasteroverride blijft herkenbaar als override.

Het originele jobbestand wordt door een override niet gewijzigd. Voor een
herhaalbare run gebruik je de sheet-regel of de expliciete CLI-opties.

## 2. Grenzen: raster en geheugen, niet alleen millimeters

Een fysieke maat zegt onvoldoende over de kosten: halvering van de celgrootte
verviervoudigt het aantal plaatcellen. De huidige loader reserveert naast de
bitboard ook uint32-counts en uint64-prefixsommen, ook als een run die later
niet allemaal gebruikt.

Conservatief voorstel voor de eerste applicatielimiet:

- Hoogstens 8192 cellen per as.
- Hoogstens 16.000.000 cellen in totaal.
- Hoogstens 256 MiB voor de berekende plaatbuffers.

Dit zijn voorgestelde productgrenzen, geen gemeten hardwaremaximum. Ze moeten
bij de implementatie worden gecontroleerd met representatieve workloads.
De rekenkundige contracts van de portable bitmapmodule blijven onafhankelijk
van deze GUI-/loaderlimieten.

Voor de huidige buffers, met W en H in cellen en S = ceil(W / 64):

```text
bitmap       = 8 * S * H bytes
counts       = 4 * W * H bytes
prefix       = 8 * (W + 1) * H bytes
row scratch  = 4 * 8 * S bytes
```

Gebruik de echte capacity-functies bij implementatie. Bereken producten en
bytes met overflowcontroles voordat iets wordt gealloceerd. Ook smalle platen
hebben woordpadding en een prefix-element extra per rij; alleen W * H tellen
is daarom onvoldoende.

De volgende tabel is uit deze huidige bufferindeling berekend:

| Plaat in mm | Cel in mm | Rastercellen | Plaatbuffers in MiB |
| --- | ---: | ---: | ---: |
| 500 x 500 | 1.0 | 250.000 | 2.90 |
| 600 x 600 | 1.0 | 360.000 | 4.17 |
| 600 x 300 | 1.0 | 180.000 | 2.09 |
| 800 x 600 | 1.0 | 480.000 | 5.56 |
| 800 x 600 | 0.25 | 7.680.000 | 88.83 |
| 1250 x 2500 | 1.0 | 3.125.000 | 36.16 |
| 1250 x 2500 | 0.5 | 12.500.000 | 144.62 |
| 1250 x 2500 | 0.25 | 50.000.000 | 578.31 |

De tabel omvat geen onderdeelmaskers, rotaties, rasteriseerwerkruimte of
GUI-/allocator-overhead. Bij atomisch herladen bestaan oude en nieuwe buffers
tijdelijk naast elkaar. Die piek moet worden meegenomen in de metingen;
een plaatbufferlimiet is geen garantie voor de totale proces-RAM.

Met de voorgestelde grenzen passen 800 x 600 mm op 0.25 mm en 1250 x 2500 mm
op 0.5 of 1 mm. De grote plaat op 0.25 mm wordt afgewezen met een concrete
melding en een passende rasterresolutie als suggestie. De applicatie wijzigt
de resolutie of maat nooit automatisch. Een geheugenlimiet garandeert ook
geen vaste rekentijd: die hangt mede af van onderdelen, rotaties en zoekmodus.

## 3. Implementatie

### A. Gedeelde maat- en capaciteitcontrole

Voeg een kleine `bp_sheet`-module toe voor omrekening mm -> cellen en de
capaciteit-/limietcontrole. Zowel GUI-voorvertoning als loader gebruiken die
berekening, zodat wat de GUI toestaat ook door de loader wordt geaccepteerd.

Controleer positieve, eindige afmetingen en celgrootte, minstens een hele cel
per as, cellimieten en bytebudget. Rapporteer ongeldige externe invoer en
capaciteitsoverschrijding met een expliciet resultaat, niet met een abort.
De plaat blijft naar beneden afgerond; bestaande conservatieve onderdeel-
rasterisatie en clearance blijven van toepassing.

### B. Loaderopties voor formaat-override

Breid laden uit met een parameterstruct: pad, expliciete optionele
plaatmaat-override, celgrootte-override en applicatielimieten. Behoud
`bp_job_load(path, cell_override)` als compatibele wrapper.

Pas de overrides toe na parsing en voor alle plaatallocaties. Valideer ook de
benodigde onderdeel-/rasteriseerbuffers voordat die grote allocaties doen.
Alle plaatbuffers moeten bij de actieve maat passen: alleen width/height in
een bestaande bitmapstruct veranderen is onjuist.

Voor de eerste versie mag Apply & Pack een vervangende job voorbereiden via
de bestaande laadroute. Dat herbouwt ook de maskers, maar gebeurt alleen bij
een geometriewijziging. Hergebruik van maskers bij uitsluitend een andere
plaatmaat kan daarna worden gemeten als gerichte optimalisatie.

Bereid de nieuwe job en GUI-buffers volledig voor voordat de oude toestand
wordt vervangen. Bij ongeldige invoer, een overschreden limiet of een
allocatiefout blijft de vorige berekening zichtbaar en exporteerbaar.

### C. GUI en CLI

Implementeer de presets en eigen invoer in de GUI. Bij succesvol toepassen:
publiceer de nieuwe indeling, zet de plaatselectie terug op de eerste plaat
en pas Fit view toe. Aantallen, bounding boxes en SVGs gebruiken de werkelijk
toegepaste maat, niet de nog gewijzigde invoervelden.

Voeg aan bp_pack en bp_gui dezelfde override toe:

```sh
--sheet-size <breedte_mm> <hoogte_mm>
```

Gebruik niet `--sheet`: die optie selecteert in de GUI al een resultaatplaat.
`--sheets` blijft het maximumaantal identieke platen.

Onderdelen die niet op de kleinere plaat passen blijven een verwacht resultaat
en verschijnen als niet-geplaatst. Ze mogen niet worden verkleind of stil
worden weggelaten.

## 4. Verificatie en documentatie

- Maat- en buffertests voor alle presets en beide plaatstanden.
- Grensgevallen: 63/64/65 cellen, precies op/over cellimiet en bytebudget,
  nul, negatieve waarden, NaN, oneindig en invoer kleiner dan een rastercel.
- Laadtests: formaat uit bestand, geldige override, afgewezen override,
  juiste prioriteit en behoud van de bestaande laad-API.
- GUI: maat wijzigen zonder het oude resultaat verkeerd te labelen; geldig
  toepassen; afwijzing behoudt resultaat; selecties blijven geldig na minder
  gebruikte platen; verkeerde invoer wordt niet stil gecorrigeerd.
- End-to-end: 800 x 600 en 1250 x 2500 mm nestresultaten, SVG-viewBox en
  fysieke maten controleren, plus overlap, grenzen en clearance.
- Meet geheugen en rekentijd op beide formaten, bij 1.0 en 0.5 mm, met dezelfde
  herhaalbare workloads. Meet ook de tijdelijke herlaadpiek.
- Draai pixi run test, de sanitizer-taak en de GUI-controles. Gebruik een eigen
  sanitizer-buildmap om bestaande gebruikersuitvoer in build/ te behouden.
- Werk beide README's bij met de presets, de betekenis van het rasterbudget,
  de CLI-optie en een GUI-afbeelding met 800 x 600 mm geselecteerd.

Acceptatie: de gebruiker kan zonder het jobbestand te bewerken 800 x 600 mm
kiezen, een rechthoekig reststuk invoeren en 1250 x 2500 mm gebruiken binnen
het gepubliceerde rasterbudget. Viewer en SVG gebruiken steeds dezelfde maat.

## 5. Metingen

`python3 tools/measure_sheet_sizes.py --gui` op `jobs/demo.job` (x86-64, WSL2;
mediaan CPU-tijd en grootste piek-RSS over drie runs, 40 vaste zoekrondes):

| Plaat in mm | Cel in mm | Plaatbuffers in MiB | Constructie CPU s | Constructie piek MiB | Zoeken CPU s | Zoeken piek MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 800 x 600 | 1.0 | 5.56 | 0.020 | 14.2 | 3.17 | 14.2 |
| 800 x 600 | 0.5 | 22.21 | 0.070 | 14.2 | 11.50 | 24.8 |
| 1250 x 2500 | 1.0 | 36.16 | 0.023 | 14.2 | 7.83 | 38.7 |
| 1250 x 2500 | 0.5 | 144.62 | 0.081 | 14.2 | 38.78 | 147.2 |

Herlaadpiek in bp_gui (40 zoekrondes per nestrun):

| Cel in mm | Start | Toegepast | Piek MiB | Piek bij direct starten MiB | Extra MiB |
| ---: | --- | --- | ---: | ---: | ---: |
| 1.0 | 800 x 600 | 1250 x 2500 | 150.5 | 150.5 | -0.1 |
| 1.0 | 1250 x 2500 | 800 x 600 | 127.5 | 120.0 | 7.5 |
| 0.5 | 800 x 600 | 1250 x 2500 | 259.1 | 259.0 | 0.0 |
| 0.5 | 1250 x 2500 | 800 x 600 | 236.7 | 136.6 | 100.1 |

Conclusies:

- Het budgetcijfer is de allocatie. Resident wordt het pas als het zoeken de
  buffers beschrijft; dan ligt de piek een paar MiB boven het budgetcijfer.
- De grenzen laten bij deze workloads ruim marge: de grootste gemeten
  configuratie (12.5 miljoen cellen) haalde 147 MiB piek in bp_pack.
- Rekentijd is de echte kostenpost: 40 rondes op 1250 x 2500 bij 0.5 mm kosten
  39 CPU-seconden. Een tijdsbudget (`--time`) blijft daarom de aangewezen
  manier om grote, fijne rasters te begrenzen.
- De herlaadpiek is hooguit de residente oude job (100 MiB van groot naar
  klein). Nieuwe plaatbuffers worden pas resident bij het nesten, nadat de oude
  job is vrijgegeven; daarom bleef de piek in alle vier gevallen onder die van
  direct starten op de grootste plaat.
- Gemeten op een workstation onder WSL2 met een enkele job. Grenzen voor
  zwaardere jobs of kleinere machines vragen een eigen meting met hetzelfde
  script.

## 6. Vervolg

Met een vrij plaatformaat is de solver bruikbaar voor productieplanning op
laser, plasma, waterstraal en frees. Voor zagen is de volgende stap
guillotineversnijding: legplannen die alleen uit doorlopende zaaglijnen van
rand tot rand bestaan, zodat een plaat- of balkzaag ze in stappen kan zagen.
Daarnaast blijven open: een voorraad met verschillende plaatmaten in een run en
onregelmatige reststukken (zie Uitgangspunt).
