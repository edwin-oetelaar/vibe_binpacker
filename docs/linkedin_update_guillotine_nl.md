# Projectupdate om te delen - de zaagsolver

Upload deze twee afbeeldingen in deze volgorde:

1. [Een gezaagde plaat uit de zaaglijst](guillotine_plate.png)
2. [De viewer in zaagmodus](gui_guillotine.png)

De eerste is 1600 x 1182 pixels, de GUI-opname 1400 x 900. Kopieer de tekst
tussen de lijnen hieronder. De uitleg met metingen staat in de README onder
[Zagen op een plaatzaag](../README.nl.md#zagen-op-een-plaatzaag).

---

Robots hebben kisten nodig.

Bij Fontys bouwen we robots, en om ze te vervoeren hadden we kisten nodig. De
studenten ontwierpen ze en maakten een zaaglijst voor de platen. Alleen hadden
ze nog niet van de guillotinemethode gehoord. De Hornbach waar we lieten zagen
had daardoor een aardige uitdaging om alle stukjes te maken.

Een plaatzaag zaagt recht door, van rand tot rand. Een zaaglijst die daar geen
rekening mee houdt, laat de puzzel over aan wie achter de zaag staat.

Dat hadden we kunnen voorkomen met onze eigen open-source solver. Dus heeft die
nu twee kanten, voor twee soorten machines:

- vrije nesting voor laser, plasma, waterstraal en frees;
- guillotineversnijding voor de plaat- en cirkelzaag: rechthoeken, elke snede
  van rand tot rand, en een zaaglijst in de volgorde waarin de zager werkt.

Elke zaaglijst wordt daarna door een apart programma nagezaagd, snede voor
snede, zonder de solver te vertrouwen. En het is meetbaar: op 500 standaardinstanties
gebruikt de zoeklaag 1,7 tot 2,6 % meer platen dan PackingSolver, de huidige
state of the art, in ongeveer een negenhonderdste van de rekentijd. Die
vergelijking hoort erbij; zonder meting is het een mening.

Daarmee is de cirkel rond: we liepen tegen een probleem aan, en we losten het
op.

En dit is precies het soort probleem waar onze engineers bij Fontys bij kunnen
helpen. Ze hoeven de algoritmen niet te bedenken of te bewijzen; dat heeft de
literatuur gedaan. Hun werk is uitzoeken wat er bestaat, kiezen, het inbouwen
in iets dat draait, en meetbaar maken wat werkt en wat niet. Ook dit deel is
grotendeels door Claude geschreven; de keuzes en de metingen waren het werk.
Dat is een andere creativiteit.

Heb je zo'n probleem op de werkvloer liggen? Dan is het uitstekend
studentenmateriaal: echte randvoorwaarden, echt materiaal, en geen antwoord
achterin het boek.

https://github.com/edwin-oetelaar/vibe_binpacker

#zaaglijst #optimalisatie #maakindustrie #onderwijs #opensource

---

## Alt-tekst bij de afbeeldingen

1. Een plaat van 2800 x 2070 mm met keukenkastonderdelen, gezaagd in stroken.
   De sneden zijn gekleurd per zaagstadium: rood voor de stroken over de volle
   breedte, blauw voor de sneden daarbinnen, groen voor het derde stadium.
   Afval is gearceerd.
2. Screenshot van de viewer in zaagmodus. Plaat 1 van 3 staat bij snede 14 van
   30; de huidige snede is geel omlijnd. Links staan de zaaginstellingen, de
   platenlijst met onderdelen, benutting en sneden per plaat, en de
   stappenschuif door de zaaglijst.
