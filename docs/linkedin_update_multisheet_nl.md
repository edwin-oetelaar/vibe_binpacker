# Projectupdate om te delen - meerdere platen

Upload deze drie afbeeldingen in deze volgorde:

1. [Overzicht: de hele opdracht op twee platen](multisheet_overview_nl.png)
2. [Voor/na: de bounding box op de laatste plaat](multisheet_comparison_nl.png)
3. [De vernieuwde GUI](gui_multisheet.png)

De eerste twee zijn 1600 x 1120 pixels; de echte GUI-opname is 1400 x 900.
Kopieer de tekst tussen de lijnen hieronder. De uitgebreide uitleg staat in
[de update met beelden](update_multisheet_nl.md).

---

Wat gebeurt er met de onderdelen die niet meer op de eerste plaat passen?

Nieuwe stap in mijn open-source nestingproject: een opdracht loopt nu door
over meerdere platen. Ook de GUI laat die hele opdracht zien.

In het voorbeeld: 76 onderdelen. De eerste plaat krijgt er 43, de andere
33 schuiven door naar plaat 2. Alles geplaatst, met 1 mm tussenruimte.

Maar ook de laatste plaat verdient aandacht. Wat je daar overhoudt kan straks
weer grondstof zijn voor een volgende opdracht.

Daarom heeft de solver drie doelen, in vaste volgorde:

- Zoveel mogelijk gevraagde onderdelen plaatsen.
- Zo weinig mogelijk platen gebruiken.
- De omsluitende rechthoek op de laatste plaat zo klein mogelijk maken.

Die laatste is breedte maal hoogte. In het kleine controlevoorbeeld op de
tweede afbeelding gaat die rechthoek van 101 x 60 naar 60 x 91 mm:
van 6060 naar 5460 mm2, 9,9% kleiner. Dezelfde onderdelen, hetzelfde aantal
platen. De materiaaloppervlakte blijft gelijk; de vorm van de resterende
ruimte verandert. Rechts ontstaat een bredere vrije strook naast het werk.

Dat vind ik de interessante stap: van een volle eerste plaat naar inzicht in
de hele opdracht en de vorm van wat er overblijft.

In de viewer kun je nu tussen platen wisselen, de bounding box en benutting
per plaat bekijken, en alle platen samen als SVG exporteren. De beelden komen
uit echte solveruitvoer en een opname van de GUI; de indelingen zijn nog eens
onafhankelijk gecontroleerd op overlap, plaatgrenzen en tussenruimte.

De code en de voorbeelden zijn vrij beschikbaar. Feedback van mensen die
dagelijks met plaatmateriaal werken is welkom.

https://github.com/edwin-oetelaar/vibe_binpacker

#nesting #optimalisatie #maakindustrie #opensource

---

## Alt-tekst bij de afbeeldingen

1. Een nestingopdracht met 76 onderdelen verdeeld over twee platen van
   1200 x 1200 mm: 43 onderdelen op de eerste plaat en 33 op de tweede.
   Gele contouren markeren de omsluitende rechthoeken.
2. Dezelfde twee onderdelen op de laatste plaat, voor en na 128 zoekrondes.
   De omsluitende rechthoek verandert van 101 x 60 naar 60 x 91 mm en wordt
   9,9 procent kleiner. Naast de smallere indeling ontstaat een bredere strook.
3. Screenshot van de vernieuwde GUI. Plaat 2 is geselecteerd, met 33 onderdelen
   binnen een gele bounding box. Links staan de plaatlijst, benutting,
   lokale stappenschuif en het totaal van 76 geplaatste onderdelen.
