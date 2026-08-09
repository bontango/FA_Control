# FA_Control — Bedienungsanleitung

Für den Betrieb am Flipper. Diese Anleitung kommt ohne technische Einzelheiten aus —
wer wissen will, wie es innen funktioniert, findet das im
[Technical Reference](TECHNICAL_REFERENCE.md) (englisch).

> **Hinweis zur Sprache:** Die Weboberfläche ist englisch. Ihre Beschriftungen werden in
> dieser Anleitung wörtlich so zitiert, wie sie auf dem Bildschirm stehen — etwa
> **CONNECT** für „Verbinden".

**Inhalt**

1. [Was FA_Control macht](#1-was-fa_control-macht)
2. [Ein- und Ausschalten](#2-ein--und-ausschalten)
3. [Was die blaue LED sagt](#3-was-die-blaue-led-sagt)
4. [WLAN einrichten](#4-wlan-einrichten)
5. [Die Seite aufrufen](#5-die-seite-aufrufen)
6. [Verbinden](#6-verbinden)
7. [Die Menüs](#7-die-menüs)
8. [Kontrolle zurückgeben](#8-kontrolle-zurückgeben)
9. [Firmware aktualisieren](#9-firmware-aktualisieren)
10. [Sicherheitshinweise](#10-sicherheitshinweise)

---

## 1. Was FA_Control macht

FA_Control ist ein **Servicegerät zur Fehlersuche**. Es sitzt dauerhaft im Flipper und
stellt im WLAN eine Webseite bereit. Über diese Seite lassen sich einzelne Lampen
schalten, einzelne Spulen auslösen, die Schalter beobachten, Sounds abspielen und die
Displays beschreiben — ohne Messgerät, ohne Aufschrauben, mit dem Handy in der Hand.

Entscheidend ist: **Solange Sie nicht ausdrücklich verbinden, spielt der Flipper ganz
normal weiter.** FA_Control mischt sich nicht von selbst ein und meldet sich auch beim
Einschalten nicht bei der Anlage an. Erst wenn Sie auf der Startseite **CONNECT** drücken
und das Spiel die Kontrolle abgibt, übernimmt FA_Control. Bis dahin ist es nur ein Gerät,
das eine Webseite anzeigt.

---

## 2. Ein- und Ausschalten

**DIP-Schalter 1 auf der kleinen Vierer-Schalterbank ist der Ein/Aus-Schalter.**

| DIP 1 | |
|---|---|
| **ON** | eingeschaltet: WLAN, Webseite, Steuerung. Die blaue LED blinkt. |
| **OFF** | ausgeschaltet: das Gerät schläft und braucht praktisch keinen Strom. |

Zum Wiedereinschalten legen Sie denselben Schalter zurück auf ON — mehr ist nicht nötig,
das Gerät wacht davon auf.

Weil FA_Control dauerhaft im Flipper verbleibt, ist **ausgeschaltet der Normalzustand**.
Eingeschaltet wird es zur Fehlersuche und danach wieder ausgeschaltet.

**Wenn Sie DIP 1 im laufenden Betrieb auf OFF legen**, gibt FA_Control zuerst die
Kontrolle an das Spiel zurück und schläft erst dann ein. Der Flipper übernimmt also sofort
wieder — Sie können den Schalter jederzeit gefahrlos umlegen, auch mitten in einer
Messung.

Die beiden Schalter DIP 3 und DIP 4 haben derzeit keine Funktion. DIP 2 betrifft nur die
blaue LED (siehe nächster Abschnitt) und bleibt im Normalfall auf OFF.

---

## 3. Was die blaue LED sagt

Das Blinkmuster verrät aus zwei Metern Entfernung, was los ist — ohne die Webseite zu
öffnen:

| Muster | Bedeutung |
|---|---|
| **langsam, 1× pro Sekunde** | eingeschaltet, aber (noch) nicht mit der Anlage verbunden |
| **schnell, 5× pro Sekunde** | verbunden — FA_Control steuert den Flipper gerade |
| **sehr schnell, 10× pro Sekunde** | kurz nach dem Einschalten, das Gerät schläft gleich ein (DIP 1 steht auf OFF) |
| **dunkel** | ausgeschaltet |

Bleibt die LED dunkel, obwohl DIP 1 auf ON steht, prüfen Sie DIP 2: steht der auf ON, ist
die Blinkanzeige absichtlich abgeschaltet. Das Gerät arbeitet dann trotzdem normal.

---

## 4. WLAN einrichten

Beim ersten Einschalten kennt FA_Control noch kein Netz und macht deshalb ein eigenes auf.

1. **DIP 1 auf ON** legen und einen Moment warten.
2. Am Handy oder Notebook nach WLAN-Netzen suchen und sich mit dem offenen Netz
   **`FA-Control`** verbinden. Ein Passwort gibt es nicht.
3. In aller Regel öffnet sich die Bedienseite **von selbst**. Falls nicht, rufen Sie im
   Browser `http://192.168.4.1` auf.
4. Auf der Seite die Kachel **06 · WI-FI** anklicken.
5. Unter **NETWORK** bei **SSID** den Namen Ihres Heimnetzes eintragen und bei
   **PASSWORD** das zugehörige Passwort.
6. **SAVE + REBOOT** drücken. Das Gerät startet neu und meldet sich in Ihrem Netz an.

Danach ist das eigene Netz `FA-Control` verschwunden — verbinden Sie Handy oder Notebook
wieder mit dem gewohnten Heimnetz.

> Erscheint das Netz `FA-Control` später erneut, hat sich FA_Control nicht anmelden können:
> meist ein Tippfehler im Passwort, ein umbenanntes Netz oder schlechter Empfang. Sie
> können die Zugangsdaten dann auf demselben Weg neu eintragen.

---

## 5. Die Seite aufrufen

Sobald FA_Control in Ihrem Heimnetz angemeldet ist, erreichen Sie es unter

**`http://fa-control.local`**

Falls Ihr Gerät diesen Namen nicht auflöst (kommt bei manchen Android-Geräten und in
manchen Firmennetzen vor), verwenden Sie die IP-Adresse. Sie steht oben im Kopf der Seite,
sobald diese einmal offen war — und Sie finden sie in der Geräteliste Ihres Routers.

Ganz oben rechts zeigt die Seite außerdem die installierte Version, die Betriebsart
(`STA` = im Heimnetz, `AP` = eigenes Netz) und die IP-Adresse an.

---

## 6. Verbinden

Die **Startseite ist der Verbindungsaufbau**. Sie sehen dort einen Balken mit dem
Verbindungszustand, drei Schaltflächen und darunter das Kachelmenü.

Die Kacheln **01 bis 05 sind grau und nicht anklickbar**, solange keine Verbindung
besteht. Das ist Absicht: FA_Control weiß erst nach dem Verbinden, wie viele Lampen,
Spulen, Schalter, Sounds und Displays Ihre Anlage überhaupt hat. Vorher gäbe es nichts zu
steuern.

**So verbinden Sie:**

1. **CONNECT** drücken.
2. Der Balken wird grün und meldet **CONTROL GRANTED**.
3. Darunter erscheint der Kasten **REPORTED HARDWARE** mit den Angaben, die Ihre Anlage
   selbst gemeldet hat: Anzahl der Lampen, Spulen, Schalter, Sounds und Displays sowie die
   Stellenzahl je Display. In der Zeile darüber stehen Gerätename, Firmware-Version und
   das erkannte Spiel.
4. Die Kacheln 01 bis 05 sind jetzt freigeschaltet, die blaue LED blinkt schnell.

**Wenn es nicht klappt**, steht der Grund im Klartext im Balken:

| Meldung | Was zu tun ist |
|---|---|
| **CONTROL DENIED - SET OPTION DIP 4 TO ON** | Die Anlage ist da, gibt die Kontrolle aber nicht frei. Beim AtariFA ist die Freigabe der **Options-DIP 4** auf der FPGA-Platine — legen Sie ihn auf ON und drücken Sie erneut **CONNECT**. |
| **CONTROL DENIED - REQUEST LINE (GPIO10) NOT SEEN** | Die Anlage antwortet, hat aber die Übernahme-Anforderung nicht bemerkt. Prüfen Sie den Sitz des Steckers zwischen FA_Control und der Platine. |
| **NO ANSWER - CHECK WIRING AND POWER** | Es meldet sich niemand. Ist die Anlage eingeschaltet? Sitzt das Verbindungskabel richtig? Läuft auf der Gegenseite überhaupt eine passende Firmware? |
| **CONTROL DENIED** | Die Anlage lehnt aus einem anderen Grund ab. Meist hilft ein Neustart der Anlage. |

Die Startseite prüft den Zustand fortlaufend nach. Holt sich die Anlage die Kontrolle von
sich aus zurück, sehen Sie das dort sofort, und die Kacheln werden wieder grau.

Die Schaltfläche **RE-INITIALIZE (0x64)** setzt die Verbindung im laufenden Betrieb neu
auf. Das brauchen Sie nur, wenn etwas aus dem Tritt geraten scheint — etwa wenn
Lampenanzeige und Wirklichkeit auseinanderlaufen.

---

## 7. Die Menüs

Alle Steuerungsmenüs erreichen Sie über das Kachelmenü der Startseite. Mit **◀ MENU**
oben links kommen Sie zurück.

### 01 · LAMPS

Ein Raster mit einer Kachel je Lampe, von 0 an durchnummeriert. **Ein Klick schaltet die
Lampe ein, der nächste wieder aus.** Eingeschaltete Lampen leuchten im Raster orange.

So finden Sie eine defekte Lampe, ohne das Spiel durchspielen zu müssen: durchklicken und
zuschauen, welche im Gehäuse dunkel bleibt.

### 02 · COILS

Ein Raster mit einer Kachel je Spule. **Ein Klick löst einen einzelnen Impuls aus** — die
Kachel blitzt kurz auf. Spulen werden ausschließlich gepulst und nie dauerhaft
eingeschaltet; das schützt sie vor dem Durchbrennen.

Darunter steht unter **PULSE TIME** die Impulsdauer in **MILLISECONDS**. Sie gilt für alle
Spulen gemeinsam. Ändern Sie den Wert und drücken Sie **APPLY** — er wird gespeichert und
gilt auch nach dem nächsten Einschalten.

> Diese Impulsdauer ist die **einzige** Einstellung, die FA_Control selbst speichert.
> Alles andere kommt von der Anlage.

### 03 · SWITCHES

Ein Raster mit einer Kachel je Schalter. Die Anzeige aktualisiert sich **einmal pro
Sekunde**; ein betätigter Schalter leuchtet grün.

Diese Seite ist **reine Anzeige**. Schalter lassen sich nicht setzen — das Protokoll
zwischen FA_Control und der Anlage kennt dafür keinen Befehl, weil ein Schalter etwas
meldet und nicht etwas ausführt. Zum Prüfen betätigen Sie den Schalter von Hand am Gerät
und schauen zu, ob die Kachel reagiert.

### 04 · SOUND

Ein Raster mit einer Kachel je Sound. **Ein Klick spielt ihn ab, ein erneuter Klick stoppt
ihn.** Es läuft immer nur ein Sound zugleich; ein neuer löst den vorherigen ab.

Meldet Ihre Anlage keine Sounds, bleibt dieses Raster leer — dann kann sie keinen Ton
ausgeben.

### 05 · DISPLAYS

Für jedes Display eine Zeile mit einer nachgebildeten Siebensegmentanzeige, einem
Eingabefeld und der Schaltfläche **SEND**. In der Beschriftung steht, wie viele Stellen
das jeweilige Display hat.

Geben Sie Ziffern ein und drücken Sie **SEND** (oder die Eingabetaste). Der Text erscheint
**rechtsbündig** auf dem echten Display des Flippers. Zulässig sind Ziffern und
Leerzeichen; ein Leerzeichen lässt die Stelle dunkel.

---

## 8. Kontrolle zurückgeben

Wenn Sie fertig sind, geben Sie die Anlage wieder frei. Dafür gibt es zwei gleichwertige
Wege:

- Auf der Startseite **RELEASE CONTROL** drücken. Der Balken wird grau, die Kacheln 01–05
  werden wieder gesperrt, die LED blinkt wieder langsam. Das Gerät bleibt eingeschaltet
  und erreichbar.
- **DIP 1 auf OFF** legen. Das gibt die Kontrolle ebenfalls zurück und schaltet FA_Control
  zusätzlich aus.

In beiden Fällen übernimmt das Spiel sofort wieder.

> Falls Sie einmal weder das eine noch das andere tun — etwa weil das Handy leer ist oder
> das WLAN ausfällt: Die Anlage merkt von selbst, dass FA_Control sich nicht mehr meldet,
> und holt sich die Kontrolle nach etwa zwei Sekunden zurück. Der Flipper bleibt also nie
> stehen.

---

## 9. Firmware aktualisieren

Neue Versionen holt sich FA_Control selbst aus dem Internet. Dafür muss das Gerät in Ihrem
Heimnetz angemeldet sein — im eigenen Netz `FA-Control` gibt es keine Internetverbindung,
und die Schaltfläche ist dann gesperrt.

1. Kachel **06 · WI-FI** öffnen, Abschnitt **FIRMWARE**. Dort steht unter **INSTALLED
   VERSION**, was gerade läuft.
2. **LOAD VERSIONS** drücken. Nach einem Moment erscheint eine Auswahlliste mit den
   verfügbaren Versionen, die neueste oben.
3. Version auswählen und **INSTALL UPDATE** drücken. Die Rückfrage bestätigen.
4. Der Fortschritt wird in Prozent angezeigt. Danach startet das Gerät neu und die Seite
   lädt sich nach etwa zehn Sekunden von selbst wieder.

**Während des Updates das Gerät nicht ausschalten und DIP 1 nicht umlegen.** Ein
abgebrochenes Update ist zwar nicht gefährlich — die alte Version bleibt erhalten und
startet wieder —, aber der Vorgang beginnt dann von vorn.

---

## 10. Sicherheitshinweise

**Nicht bei laufendem Spiel verbinden.** Sobald FA_Control die Kontrolle übernimmt, gibt
das Spielprogramm sie ab: eine laufende Partie ist damit beendet, und Lampen, Spulen und
Displays stehen so, wie FA_Control sie setzt. Verbinden Sie im Ruhezustand der Anlage.

**Spulen mit Bedacht auslösen.** FA_Control pulst Spulen nur kurz und schaltet sie nie
dauerhaft ein. Trotzdem ist eine Spule ein kräftiger Elektromagnet: greifen Sie beim
Auslösen nicht ins Spielfeld, und lösen Sie nicht in schneller Folge immer wieder dieselbe
Spule aus — sie wird warm.

**Vorsicht bei langen Impulszeiten.** Der Wert unter **PULSE TIME** gilt für alle Spulen.
Ein zu hoher Wert belastet die Spule stärker als im Spielbetrieb vorgesehen. Erhöhen Sie
ihn nur, wenn Sie wissen, warum, und stellen Sie ihn danach wieder zurück.

**Der Flipper steht unter Netzspannung.** Diese Anleitung beschreibt nur die Bedienung von
FA_Control. Arbeiten im geöffneten Gerät setzen die üblichen Vorsichtsmaßnahmen voraus.
