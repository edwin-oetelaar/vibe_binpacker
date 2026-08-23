# De printplaat van vijf euro ging niet vanzelf

*Wat panelisatie, plaatmateriaal en dertig jaar ervaring met elkaar te maken
hebben — en waar AI in dat plaatje past.*

**Omslagafbeelding:** `docs/comparison_nl.png`

---

Bestel vandaag vijf printplaten en je bent minder kwijt dan wat vroeger alleen
al aan opstartkosten gerekend werd. Voor wie er van buitenaf naar kijkt, is dat
gewoon hoe het werkt. Prijzen dalen, techniek wordt beter, klaar.

Wie op de werkvloer heeft gestaan weet dat het anders zit.

## Wat er onder zit

Die prijs bestaat omdat iemand het probleem heeft opgelost hoe je kleine orders
van verschillende klanten samen op één productiepaneel krijgt. Niet zomaar
erop, maar zó dat de doorlooptijd klopt, de uitval laag blijft, en het paneel
ook nog door de rest van het proces past.

Ik heb daaraan gewerkt voor een klant in de PCB-industrie, in een tijd dat dit
nog niet gewoon was. Bedrijven als EuroCircuits waren er vroeg bij en hebben
laten zien wat er mogelijk werd zodra je het paneel serieus ging optimaliseren.
Wat mij daarvan is bijgebleven, is niet het algoritme. Het is hoeveel er
omheen zat: welke orders je überhaupt mag combineren, wat een testkupon kost
aan ruimte, hoe je omgaat met verschillende koperdiktes en doorlooptijden, wat
er gebeurt als een order uitvalt nadat het paneel al is ingepland.

Een optimalisatie die dat allemaal negeert kan aantoonbaar de beste zijn en
alsnog geld kosten.

## Hetzelfde probleem, ander materiaal

Onlangs heb ik dat probleem opnieuw gebouwd, in de open lucht ditmaal: een
2D-nesting solver die onderdelen zo op een plaat legt dat er zo min mogelijk
overblijft. Willekeurige vormen, gaten, kwartslagen, en een gegarandeerde
tussenruimte tussen de onderdelen. Plaatwerk in plaats van printplaten, maar
het is dezelfde vraag: hoe krijg je zoveel mogelijk waarde uit één stuk
grondstof.

De afbeelding hierboven vertelt het in één oogopslag. Links het resultaat van
één doorloop van een klassieke constructieheuristiek. Rechts hetzelfde na tien
seconden zoeken. Dezelfde onderdelen, dezelfde plaat, 99 mm meer materiaal dat
als één rechte strook overblijft in plaats van als snippers. Die 99 mm is
precies het verschil tussen restmateriaal dat terugkomt in het magazijn en
restmateriaal dat in de container gaat.

## De vier metingen die alles omgooiden

Dit is het deel waar het mij om gaat, en het is ook het deel dat je nergens
ziet als je alleen naar het eindresultaat kijkt.

**De slimme sprong was 69 keer trager.** Bij het zoeken naar een vrije plek kun
je, als je ergens op stuit, zo ver vooruitspringen als al je informatie
toelaat. Klinkt duidelijk beter dan stoppen bij het eerste obstakel. Gemeten:
die slimme variant scheelde 0,4 % minder posities om te bekijken en kostte
zeventig keer zoveel rekenwerk per positie. De domme variant won met factor 69.

**De "echte motor" verloor.** Het ontwerp wees morfologische erosie aan als de
kern van de aanpak. Gemeten bleek die trager dan een gewone scan voor het
vinden van de eerste vrije plek — en elf tot dertig keer sneller voor het
opsommen van álle vrije plekken. Dat is een andere vraag. Het onderdeel is
gebleven, maar op een andere plek in het geheel dan waarvoor het bedoeld was.

**De tussenruimte klopte pas toen iets van buiten hem natrok.** De solver
rekende de afstand tussen onderdelen tussen celmiddelpunten. Twee cellen die
diagonaal naast elkaar liggen zitten op √2 afstand van middelpunt tot
middelpunt — maar op nul afstand van materiaal tot materiaal. Ze raken elkaar
in een hoekpunt. Geen enkele van mijn eigen tests ving dat, want die had ik op
hetzelfde verkeerde idee gebaseerd. Een los controleprogramma dat de uitvoer
teruglas en zelf natrok, ving het meteen.

**Een gulzige herstellus komt nooit van zijn plek.** Een van de zoeklagen
haalt onderdelen weg en legt ze opnieuw. Vijfduizend rondes lang leverde dat
precies nul verbeteringen op. Pas toen het opruimen een aaneengesloten gebied
leeg ging maken én het opnieuw leggen af en toe een kandidaat mocht overslaan,
kwam het los.

Vier van de zes. De andere twee staan in de repository, met de cijfers erbij.

## En daar zit AI in het verhaal

Ik heb voor dit project bijna geen regel code getypt. Claude Opus 5 deed dat,
en in een tempo dat ik met de hand niet in de buurt kom. Ik bepaalde de
richting, koos de aanpak, en beoordeelde wat eruit kwam.

Dus je zou kunnen zeggen: dan kan iedereen dit.

Kijk dan nog eens naar die vier metingen. In elk van die gevallen was de voor
de hand liggende aanpak overtuigend, goed onderbouwd en fout. Het gereedschap
had er elke keer de plausibele versie van gemaakt, en die had gecompileerd,
gedraaid en er prima uitgezien. Wat het verschil maakte was niet het typen. Het
was de gewoonte om te zeggen: laat eens een getal zien. En het was weten wélk
getal je moet vragen.

Dat is dertig jaar ervaring, en die wordt door dit gereedschap niet overbodig.
Hij wordt er hooguit meer waard, omdat de tijd tussen "ik vermoed iets" en "ik
weet het" van dagen naar minuten gaat. Ik heb in dit project meer geleerd over
nesting dan in de jaren dat ik het met de hand deed, simpelweg omdat ik veel
meer heb kunnen uitproberen.

Ik geef daarnaast parttime les bij Fontys ICT, en daar verschuift de grond
minstens zo hard. Dat is ook waarom ik dit soort dingen zelf blijf bouwen. Je
kunt niet uitleggen wat er verandert als je het niet zelf in handen hebt gehad.

## Waar een algoritme ophoudt

Een nesting-algoritme optimaliseert één ding: hoe de onderdelen op één plaat
liggen. Dat is zelden waar het meeste geld zit.

Waar het wél zit: welke orders je samen laat produceren, welke formaten je
inkoopt, of restmateriaal met een bruikbare vorm terugkomt of verdwijnt,
walsrichting, snijvolgorde, klemzones, charges en traceerbaarheid. En altijd:
wat je operators werkelijk doen als het plan hun niet uitkomt.

En dan moet het nog ergens landen — in je ERP of MES, je CAM-postprocessor,
jouw tekeningconventies. Dat deel is niet spannend en het is wel het meeste
werk.

Daar ligt mijn vak. Ik ben afgestudeerd in technische bedrijfskunde en
operations research, en ik vind dit soort puzzels na al die jaren nog steeds
leuk. Maar het rendement zit niet in de puzzel. Het zit in het begrijpen van
het proces eromheen, en in het meten of het werkt.

De hele repository staat open, MIT-licentie, inclusief de uitkomsten die
tegenvielen. Een repository die alleen laat zien wat werkte, vertelt je bar
weinig over hoe je er komt.

**github.com/edwin-oetelaar/vibe_binpacker** — met een README in het
[Nederlands](https://github.com/edwin-oetelaar/vibe_binpacker/blob/main/README.nl.md)
en het
[Engels](https://github.com/edwin-oetelaar/vibe_binpacker/blob/main/README.md).

Snij je zelf onderdelen uit voorraadmateriaal, of combineer je orders op een
paneel, en heb je het vermoeden dat daar nog wat te halen valt? Ik praat er
graag over door.

**Edwin van den Oetelaar** — zelfstandig softwareontwikkelaar
edwin@oetelaar.com · 06 5332 9077 · [oetelaar.com](https://www.oetelaar.com/)
