# Plan: guillotineversnijding voor plaat- en balkzagen

Status: ontwerp, nog niet geimplementeerd. Roadmap: P9 in `PLAN.md`.

## Uitgangspunt

De huidige solver maakt vrije nesting: onderdelen liggen waar ze passen, met
gegarandeerde tussenruimte. Dat is goed voor laser, plasma, waterstraal en
frees, die elke baan kunnen volgen. Een plaat- of balkzaag kan dat niet. Hij
maakt alleen rechte sneden die van rand tot rand door het stuk lopen dat op dat
moment op de tafel ligt. Een legplan is zaagbaar als het in stappen zo kan
worden opgedeeld: eerst de plaat in stroken, dan de stroken in vakken, dan de
vakken in onderdelen.

Dit plan voegt een guillotinemodus toe. De uitkomst is niet alleen een legplan,
maar ook een zaaglijst: welke snede, in welke volgorde, op welke maat. Wat niet
zaagbaar is, mag de modus niet opleveren.

Buiten de scope van deze eerste versie: niet-rechthoekige onderdelen, defecten
in de plaat (glas), het optimaliseren van pakketzagen (meerdere platen tegelijk)
en het direct aansturen van een zaagbesturing.

## 1. Het probleem in de taal van de literatuur

Dit is tweedimensionaal bin packing met guillotinesneden en een begrensd aantal
stadia (Gilmore & Gomory 1965 introduceerden de staged-indeling). De varianten
die ertoe doen:

- **Aantal stadia.** Bij 2 stadia: stroken, dan onderdelen. Bij 3 stadia
  (gangbaar op plaatzagen, en ook de eis in de ROADEF/EURO Challenge 2018 van
  Saint-Gobain): stroken, vakken, onderdelen.
- **Exact of met nakap.** Exact betekent dat elk onderdeel na het laatste
  stadium precies op maat is. Met nakap mag een extra snede een onderdeel op
  maat brengen; die snede telt niet als stadium.
- **Rotatie.** Vast (houtnerf, decor) of een kwartslag toegestaan.
- **Zaagsnede (kerf).** Elke snede kost de breedte van het zaagblad.

Lodi, Martello & Vigo (1999) behandelen precies deze familie: vaste of
draaibare onderdelen, met of zonder guillotine-eis, met een heuristiek per
variant en een gezamenlijk tabu-search-raamwerk. Charalambous & Fleszar (2011)
bouwen een oplossing plaat voor plaat op (bin-oriented), snel genoeg voor grote
instanties. De sterkste algemene aanpak is de anytime tree search van
Libralesso & Fontan, die de ROADEF/EURO Challenge 2018 won en als PackingSolver
open source beschikbaar is.

## 2. Ontwerpbeslissingen

### 2.1 Geen raster, maar exacte maten

De bitboard is gebouwd voor vrije vormen. Voor zagen is het de verkeerde
representatie:

- Een zaag werkt op exacte maten en de zaagsnede is vaak een gebroken maat,
  bijvoorbeeld 4,4 mm. Het raster rondt conservatief af en verliest per rand tot
  een cel.
- De guillotine-eis is een eigenschap van de structuur, niet van posities. Een
  boom van sneden garandeert hem per constructie. Achteraf op een raster
  controleren of iets zaagbaar is, is duurder en moeilijker te bewijzen.

De guillotinemodus werkt daarom met gehele lengtes in eenheden van 0,01 mm, als
`uint32_t` met namen die eindigen op `_10um` (bijvoorbeeld `width_10um`). Dat
is exact voor gangbare zaagsneden, deterministisch, past op embedded
toolchains, en loopt tot 42,9 m: ruim genoeg voor jumboplaten van 5600 mm.
Onderdelen ronden naar boven af en platen naar beneden, net als nu.

### 2.2 Het legplan is een boom van sneden

```text
plaat                 na randbesnoeiing
  strook              stadium 1: sneden over de volle breedte
    vak               stadium 2: sneden over de volle strookhoogte
      onderdeel       stadium 3: sneden over de volle vakbreedte
        (nakap)       optioneel: brengt een onderdeel op maat
```

Elke knoop is een rechthoek met een positie in zijn ouder, en elke overgang
van ouder naar kinderen is een reeks evenwijdige sneden van rand tot rand. De
zaagbaarheid volgt dus uit de vorm van de boom. De zaaglijst is een wandeling
door die boom.

De richting van de eerste snede is een machine-eigenschap: langs de lengte,
langs de breedte, of vrij te kiezen per plaat.

### 2.3 Zaagsnede, randbesnoeiing en minimaal afval

- **Zaagsnede:** een snede op positie p verbruikt [p, p + kerf). Een stuk dat
  tot de rand van zijn ouder reikt heeft daar geen snede nodig.
- **Randbesnoeiing:** per rand een vaste strook die eraf gaat, omdat
  plaatranden beschadigd of niet haaks zijn. Telt niet als stadium.
- **Minimaal afval:** een zaag kan geen reepje afval afzagen dat smaller is dan
  een minimale maat; het breekt of klemt. Afvalstukken zijn dus 0 breed of
  minstens `min_waste`. De ROADEF-opgave kent dezelfde eis.

`clearance` uit de vrije nesting en `kerf` betekenen in deze modus hetzelfde
(materiaal tussen twee buren), maar komen niet samen voor. Een job met beide in
guillotinemodus wordt gemeld als conflict, niet stil omgerekend.

### 2.4 Alleen rechthoeken

Een zaag maakt rechthoeken. In guillotinemodus moet elk onderdeel een `rect`
zijn. Een `poly` wordt gemeld als niet zaagbaar en niet stil vervangen door
zijn omsluitende rechthoek. Of dat laatste later een expliciete optie wordt, is
een open vraag. Het `turn`-veld van `rect` regelt de nerf: 0 betekent vaste
richting.

### 2.5 Doelfunctie

Lexicografisch, in lijn met de multi-plaatmodus:

1. zoveel mogelijk gevraagde onderdelen geplaatst,
2. zo min mogelijk platen,
3. op de laatste plaat een zo groot mogelijke bruikbare reststrook: het stuk
   dat met een enkele snede van stadium 1 vrijkomt. Voor de zaag is een
   doorlopende reststrook terug te leggen in het rek; versnipperd afval niet.

Daarna als tiebreak: minder sneden. Dat is zaagtijd.

## 3. Modules

Volgens de huisstijl: kleine modules, een header per module, buffers van de
aanroeper, `BP_REQUIRE` aan de rand.

| module | verantwoordelijkheid |
| --- | --- |
| `bp_cutplan` | de boom van sneden in 0,01 mm, de zaaglijst als wandeling, en `bp_cutplan_check`: een onafhankelijke controle die een getagd resultaat teruggeeft |
| `bp_guillotine` | constructie en zoeken: van onderdelen en platen naar een `bp_cutplan` |
| `bp_job` (uitbreiding) | rechthoekmaten per onderdeel bewaren; nieuwe regels parsen; maskers overslaan als alleen guillotine nodig is |
| `bp_svg` (uitbreiding) | zaaglijnen per stadium, afval gearceerd, reststrook gemarkeerd |

Ongeldige invoer van de aanroeper (een boom met verkeerde verwijzingen, te
kleine buffers) is een contractbreuk. Een onderdeel dat niet op de plaat past,
of een job zonder zaagbaar onderdeel, is een verwacht resultaat en wordt
gemeld.

## 4. Fasering

| fase | inhoud | klaar als |
| --- | --- | --- |
| G0 | `bp_cutplan`: model, zaaglijst, controle; `tools/check_cutplan.py` die de zaaglijst zelf naloopt | handgemaakte plannen worden goed- en afgekeurd op elke regel uit 2.2 en 2.3, ook rond zaagsnede-op-de-rand en minimaal afval; bronnen staan in `RESEARCH.md` |
| G1 | jobregels en CLI: `bp_pack --guillotine`, zaaglijst als CSV, SVG met zaaglijnen | een met de hand opgebouwd plan gaat van job naar CSV en SVG, en de Python-controle keurt het goed |
| G2 | constructieve heuristiek, plaat voor plaat, drie stadia met nakap | elk resultaat op de benchmarkset is zaagbaar volgens beide controles; eerste meting van platen en afval |
| G3 | zoeken: volgorde en rotatie verstoren met G2 als decoder, met hetzelfde budgetmodel (`--search`, `--time`, vaste seed) als de multi-plaatmodus | herhaalbaar, en gemeten beter dan G2 op de benchmarkset |
| G4 | viewer: modusschakelaar, zaaglijnen per stadium, de stapschuif speelt de zaagvolgorde af | `tools/check_gui_guillotine.py` controleert pixels, SVG en zaaglijst zoals de bestaande GUI-checks |
| G5 | alleen bij gemeten noodzaak: tree search in de stijl van Libralesso & Fontan | het gat met de referentie uit sectie 5 is kleiner geworden, en de kosten zijn gemeten |

G0 komt eerst, om dezelfde reden als `bp_bitmap` toen: alles wat daarna komt
leunt op een controle die niet van de solver afhangt.

### G2 in meer detail

Een constructieve heuristiek, deterministisch en zonder allocatie in de lus:

1. Sorteer de onderdelen, bijvoorbeeld op hoogte en dan breedte. Bij
   toegestane rotatie kiest de heuristiek de stand per plaatsing.
2. Open een plaat. Open een strook met de hoogte van het eerste onderdeel dat
   nog past.
3. Vul de strook met vakken. De breedte van een vak is die van zijn eerste
   onderdeel. Stapel daarin onderdelen van dezelfde breedte, of smaller met
   nakap, tot de strook vol is.
4. Kies telkens het onderdeel dat het minste afval laat (best fit), met de
   eis dat afval 0 of minstens `min_waste` is.
5. Is de strook vol, open dan een nieuwe; past er geen strook meer, open dan
   een nieuwe plaat.

Varianten om te meten, niet vooraf te kiezen: sorteervolgorde, first fit
tegenover best fit, en het vullen van een strook als klein knapsackprobleem.

## 5. Verificatie en metingen

- Unittests per module, met de randgevallen: onderdeel precies even breed als
  het paneel (geen zaagsnede nodig), zaagsnede die precies wel of niet meer
  past, afval van `min_waste` - 1, `min_waste` en 0, rotatie verboden en
  toegestaan, en de grens van `uint32_t` in 0,01 mm.
- Death tests voor elke contractbreuk.
- `tools/check_cutplan.py` leest alleen de CSV-zaaglijst en controleert
  opnieuw: elke snede loopt van rand tot rand in zijn paneel, het aantal stadia,
  de zaagsnede, het minimale afval, en de maten en aantallen van de
  onderdelen. Zoals `check_layout.py` hangt hij niet af van de solver.
- **Benchmarkset:** de gangbare klassen voor 2D bin packing (Berkey & Wang
  1987; Martello & Vigo 1998; in G0 bij de bron verifieren), plus de bestaande
  jobs in `jobs/` met alleen rechthoeken.
- **Referentie:** PackingSolver (Fontan & Libralesso) op dezelfde instanties,
  als externe meetlat in een script en niet als afhankelijkheid. De licentie
  controleren voordat er iets van wordt overgenomen.
- **Prijs van zaagbaarheid:** dezelfde rechthoekjobs vrij genest en
  guillotine-genest. Meet het verschil in platen en afval. Dat is het getal
  dat een werkplaats wil weten.
- Rekentijd en geheugen zoals in `tools/measure_sheet_sizes.py`: herhaalbaar,
  met vaste rondes, op een vaste machine.

## 6. Uitvoer voor de werkvloer

- **Zaaglijst (CSV):** plaat, volgnummer, stadium, ouderpaneel, richting,
  positie vanaf de oorsprong van het paneel, lengte van de snede. Plus een
  onderdelenlijst met label, plaat, positie en of het gedraaid is.
- **SVG per plaat:** onderdelen met label, zaaglijnen per stadium in eigen
  kleur en dikte, afval gearceerd, reststrook met maat.
- **Herhaling:** platen met een identiek zaagpatroon worden als patroon x
  aantal gemeld. Dat is de opstap naar pakketzagen; die later optimaliseren is
  een eigen fase.

Een exportformaat voor een specifieke zaagbesturing hoort in de Python-tooling,
op basis van de CSV, zodra duidelijk is welke besturing het moet worden.

## 7. Open vragen voor de werkplaats

Deze antwoorden bepalen de standaardwaarden, niet het ontwerp:

1. Welke zaag: plaatzaag of balkzaag, en hoeveel stadia kan hij praktisch?
2. Breedte van de zaagsnede, en de randbesnoeiing per rand.
3. Kan de eerste snede beide kanten op, of altijd langs de lengte?
4. Minimale afvalbreedte die de zaag nog netjes afzaagt.
5. Welke materialen hebben een nerf of decor waardoor draaien verboden is?
6. Vanaf welke maat is een reststrook de moeite waard om te bewaren?
7. Wordt er in pakketten gezaagd, en hoe hoog maximaal?
8. Moeten niet-rechthoekige onderdelen als hun omsluitende rechthoek worden
   voorgezaagd, met de vorm daarna op een andere machine?
9. Welk bestandsformaat leest de zaagbesturing?

## 8. Risico's

- **Meer afval dan vrije nesting.** Dat is de prijs van zaagbaarheid. Het
  verschil wordt gemeten en in de README gezet, niet verzwegen.
- **Scope.** Defecten, pakketoptimalisatie en gemengde plaatmaten zijn echte
  vragen, maar elk een eigen fase. Ze worden pas gebouwd als een werkplaats ze
  nodig heeft.
- **Twee modellen naast elkaar.** Raster voor vrije nesting, exacte boom voor
  zagen. Dat is bewust: ze delen de job, de plaatmaat, de multi-plaatlogica en
  de uitvoer, en niet hun geometrie.

## Bronnen

- P. C. Gilmore, R. E. Gomory, *Multistage Cutting Stock Problems of Two and
  More Dimensions*, Operations Research 13(1):94-120, 1965.
  <https://doi.org/10.1287/opre.13.1.94>
- A. Lodi, S. Martello, D. Vigo, *Heuristic and Metaheuristic Approaches for a
  Class of Two-Dimensional Bin Packing Problems*, INFORMS Journal on Computing
  11(4):345-357, 1999. <https://doi.org/10.1287/ijoc.11.4.345>
- C. Charalambous, K. Fleszar, *A constructive bin-oriented heuristic for the
  two-dimensional bin packing problem with guillotine cuts*, Computers &
  Operations Research 38(10), 2011. <https://doi.org/10.1016/j.cor.2010.12.013>
- Saint-Gobain / ROADEF, *Challenge ROADEF/EURO 2018: Cutting Optimization
  Problem Description*, 2018.
  <https://www.roadef.org/challenge/2018/files/Challenge_ROADEF_EURO_SG_Description.pdf>
- L. Libralesso, F. Fontan, *An anytime tree search algorithm for the 2018
  ROADEF/EURO challenge glass cutting problem*, EJOR 2021.
  <https://arxiv.org/abs/2004.00963>
- F. Fontan, L. Libralesso, *PackingSolver: a solver for packing problems*,
  2020. <https://arxiv.org/abs/2004.02603>. Code en documentatie:
  <https://fontanf.github.io/packingsolver>
- J. Jylanki, *A Thousand Ways to Pack the Bin*, 2010 (al in `RESEARCH.md`):
  guillotine met vrije rechthoeken, als eenvoudige vergelijkingsbasis.
