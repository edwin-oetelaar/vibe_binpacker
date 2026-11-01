# De printplaat van vijf euro ging niet vanzelf

*Wat panelisatie, plaatmateriaal en dertig jaar ervaring met elkaar te maken
hebben — en waar AI, en een klas vol studenten, in dat plaatje passen.*

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

Deze zomervakantie heb ik dat probleem opnieuw gebouwd, dit keer open source:
een 2D-nesting solver die onderdelen zo op een plaat legt dat er zo min mogelijk
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

## Vier jaar, en meerdere goede antwoorden

Deze week gaat het gebouw weer open en begint er een nieuwe lichting aan
hun opdrachten. Drie dagen per week sta ik dan voor de klas bij Fontys ICT, bij
software engineering en informatietechnologie. Niet op een campus, maar in het
Fontys ICT InnovationLab op Strijp-T in Eindhoven: zo'n 4.000 m² waar onderwijs,
onderzoek en bedrijfsleven op dezelfde vloer zitten, en waar studenten met
onderzoekers en externe partners aan echte vraagstukken werken. Zelf zit ik
vooral in het Smart Systems Lab, waar ik aan robots werk en aan perceptie met
camera's en AI.

De andere twee dagen bouw ik zelf, op het grensvlak van wat er kan, en het
mooiste vind ik het als dat de praktijk haalt. Die verdeling houd ik al jaren
aan, omdat de ene helft de andere scherp houdt. Je kunt niet uitleggen wat er
verandert als je het niet zelf in handen hebt gehad.

We hebben vier jaar met deze studenten. Vier jaar om drie dingen over te
brengen die een stuk lastiger zijn dan welk algoritme ook: dat een probleem
meestal meer dan één goed antwoord heeft, dat je fouten moet maken om iets te
leren, en dat voortgang boeken doorgaans meer waard is dan wachten op het
perfecte antwoord.

Dit project is wat er van mijn zomervakantie is gekomen, en het is precies die
les, met de cijfers erbij. Zes keer bleek de voor
de hand liggende aanpak fout. Dat staat er allemaal in, want de verkeerde
afslagen zijn nu juist het deel waar je iets van leert. Wie alleen
eindresultaten ziet, leert dat software in één keer goed komt. Dat is de
duurste illusie die je iemand mee kunt geven.

We zitten in Brainport, waar de theorie van informatica en wiskunde grenst aan
een regio die daadwerkelijk dingen máákt. Praktische engineers zijn wat die
twee werelden verbindt, en daar zit de impact. Ik doe dat met een team van
internationale collega's die allemaal zelf in het vak hebben gestaan — daarom
kunnen ze studenten vertellen wat er echt gebeurt in plaats van wat er hoort
te gebeuren. Het is leuk werk, en het is serieus werk.

Zit je in de regio en wil je zien wat er in dat lab gebeurt: het is een open
omgeving, en langskomen kan gewoon.

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

De hele repository staat open, onder de AGPL, inclusief de uitkomsten die
tegenvielen. Een repository die alleen laat zien wat werkte, vertelt je bar
weinig over hoe je er komt.

**github.com/edwin-oetelaar/vibe_binpacker** — met een README in het
[Nederlands](https://github.com/edwin-oetelaar/vibe_binpacker/blob/main/README.nl.md)
en het
[Engels](https://github.com/edwin-oetelaar/vibe_binpacker/blob/main/README.md).

Snij je zelf onderdelen uit voorraadmateriaal, of combineer je orders op een
paneel, en heb je het vermoeden dat daar nog wat te halen valt? Ik praat er
graag over door.

**Edwin van den Oetelaar** — docent bij Fontys ICT, uitvinder en zelfstandig
softwareontwikkelaar

Commercieel werk: edwin@oetelaar.com · 06 5332 9077 ·
[oetelaar.com](https://www.oetelaar.com/) ·
[LinkedIn](https://www.linkedin.com/in/oetelaar/)

Studentenprojecten: e.vandenoetelaar@fontys.nl · Fontys ICT InnovationLab,
gebouw TQ, Achtseweg Zuid 151C, 5651 GW Eindhoven
