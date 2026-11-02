# DSiDL

Een grafische app met aanraakscherm: lijst met kaarten, zoeken met een eigen schermtoetsenbord, tabbladen per soort, voortgang met snelheid en resterende tijd, en een QR-code om je telefoon te verbinden.

Homebrew-downloader voor de (gemodde) Nintendo DSi. Alles werkt op de DSi zelf via wifi:

- **Lijst + zoeken**: haalt de homebrew-catalogus van [Universal-DB](https://db.universal-team.net) op (dezelfde bron als Universal-Updater), toont alle DS/DSi-titels met een `.nds`- of `.dsi`-bestand en laat je zoeken en filteren.
- **Downloaden naar een map naar keuze**: standaard `sd:/roms/nds/`; druk bij het downloaden op **Y** om door je SD-kaart te bladeren en een andere map te kiezen (of een nieuwe te maken). DSiDL onthoudt de laatst gekozen map.
- **Bestanden beheren vanaf je telefoon**: de DSi wordt een mini-webserver. Open het getoonde adres (bv. `http://192.168.1.23`) in de browser van je telefoon: blader door de hele SD-kaart, upload naar elke map, download naar je telefoon, maak mappen, hernoem, verwijder, en verberg of toon bestanden/mappen (bv. een map die je in TWiLight Menu++ verborgen hebt).
- **Wifi-beheer**: zoek netwerken, kies er een en typ het wachtwoord met het schermtoetsenbord. Netwerken worden bewaard in `sd:/_dsidl/wifi.txt` (niet in het interne geheugen van de DSi) en DSiDL verbindt daarna automatisch met het sterkste bekende netwerk. De netwerken uit de DSi-instellingen werken ook gewoon.
- **Zichzelf updaten**: via het menu haalt DSiDL de nieuwste versie van GitHub en vervangt zijn eigen `.nds`-bestand.

## Installeren

1. Zet `dsidl.nds` op je SD-kaart, bijvoorbeeld in `sd:/roms/nds/` of in de hoofdmap.
2. Zorg dat er wifi is ingesteld in de **DSi-instellingen** (Internet → Verbindingsinstellingen). In DSi-modus werkt WPA/WPA2 ook.
3. Start DSiDL vanuit TWiLight Menu++ (of een andere loader).

De eerste keer haalt hij de lijst op (±2 MB, duurt even). Daarna gebruikt hij de opgeslagen lijst en ververst die automatisch één keer per dag.

## Knoppen

| Knop | Actie |
|---|---|
| ↑ ↓ | door de lijst |
| ← → | pagina omhoog/omlaag |
| A | downloaden (daarna Y = andere map kiezen) |
| L / R | ander bestand kiezen als een titel er meerdere heeft (bv. `.nds` en `.dsi`) |
| Y | zoeken (toetsenbord op het onderste scherm; ook: tik op het zoekveld) |
| B | zoekopdracht wissen / annuleren |
| SELECT | soort wisselen (of tik op de tabbladen) |
| X | menu: telefoon (bestanden beheren), wifi-netwerken, lijst vernieuwen, DSiDL updaten |
| START | afsluiten |

Alles werkt ook met het aanraakscherm: tik op een titel om hem te kiezen, tik nog een keer om te downloaden, sleep de lijst om te scrollen.

**Toetsenbord:** tik op toetsen of gebruik de pijltjes en A. B wist (bij een leeg veld: terug), START is klaar, SELECT hoofdletters, X toont/verbergt een wachtwoord.

Houd **B** ingedrukt tijdens het opstarten om niet automatisch te verbinden.

In het wifi-scherm: **A** verbinden, **Y** wachtwoord (opnieuw) invoeren, **X** opgeslagen netwerk vergeten, **R** opnieuw zoeken, **START** (bij het typen) wachtwoord tonen/verbergen. `*` = opgeslagen in DSiDL, `D` = staat in de DSi-instellingen.

## Bestanden beheren vanaf je telefoon

Menu (X) → *Telefoon: bestanden beheren*. Op het onderste scherm staat een adres. Telefoon op hetzelfde wifi-netwerk → adres in de browser openen. Tik op een map om erin te gaan, op ⋮ voor acties (downloaden, hernoemen, verplaatsen, kopiëren, verbergen/zichtbaar maken, verwijderen), op 👁 om verborgen items te zien en op *Uploaden* om bestanden naar de huidige map te sturen. Bij *Verplaatsen naar...* of *Kopiëren naar...* blader je naar de doelmap en tik je op *Hierheen*; mappen worden met inhoud gekopieerd en bij dubbele namen komt er `(2)` achter. Mappen kunnen alleen verwijderd worden als ze leeg zijn; `_nds` en `_dsidl` zijn beschermd. Er is een voortgangsbalk; een bestaand bestand met dezelfde naam wordt vervangen. De DSi-wifi is traag (reken op enkele honderden KB/s), dus een groot bestand kan een paar minuten duren.

## Instellingen

`sd:/_dsidl/settings.txt` (wordt automatisch gemaakt):

```
save_dir=sd:/roms/nds
update_url=https://github.com/smikkelvriend/dsidl/releases/latest/download/
```

## Nieuwe versie uitbrengen

De update-functie kijkt naar `<update_url>version.txt` en downloadt `<update_url>dsidl.nds`.

1. Verhoog `APP_VERSION` in de `Makefile` (bv. `1.0.5`) en push naar `main`.
2. De GitHub Action (`.github/workflows/build.yml`) bouwt de app en maakt automatisch de release `v1.0.5` met `dsidl.nds` en `version.txt`.
3. Op de DSi: X → *DSiDL updaten*. Hij vergelijkt de versie, downloadt, controleert of het echt een DS-rom is en vervangt zichzelf (de oude versie wordt teruggezet als dat mislukt). Daarna sluit hij af; start hem opnieuw.

Andere repo-naam? Pas `UPDATE_BASE` in `source/main.c` aan, of `update_url` in `settings.txt`.

## Zelf bouwen

Nodig: [BlocksDS](https://blocksds.skylyrac.net) (of de Docker-image `skylyrac/blocksds:slim-latest`).

```sh
sh mbedtls-port/build-mbedtls.sh   # bouwt Mbed TLS voor de DS (eenmalig)
make APP_VERSION=1.0.1
```

## Hoe het werkt

- `source/http.c`: HTTP/HTTPS-client (dswifi-sockets + Mbed TLS 3.6, TLS 1.2, certificaatcontrole met de ingebouwde Mozilla-rootcertificaten in `data/cacert.bin`), volgt doorverwijzingen (GitHub-releases).
- `source/catalog.c`: kleine JSON-parser voor Universal-DB; houdt alleen DS-titels met een direct installeerbaar `.nds`/`.dsi`.
- `source/server.c`: webserver op poort 80 met uploadpagina.
- Netwerkwerk draait in een aparte cothread, zodat het scherm blijft reageren en B annuleert.

## Beperkingen

- Titels die alleen als `.zip`/`.7z` worden aangeboden (bv. TWiLight Menu++, nds-bootstrap) staan niet in de lijst; uitpakken op de DSi zit er (nog) niet in.
- De DSi heeft geen klok met zekere tijd, dus de geldigheidsdatum van certificaten wordt niet gecontroleerd (de handtekening en de servernaam wel).

Licenties: Mbed TLS (Apache-2.0), rootcertificaten van Mozilla (MPL-2.0), BlocksDS/libnds (zlib), lettertype Poppins (SIL Open Font License), QR Code generator van Project Nayuki (MIT).

Ontwerp bekijken zonder DSi: `cc -Isource tools/preview/host.c source/gfx.c source/ui.c source/font_data.c source/qrcodegen.c -o preview && ./preview` maakt van elk scherm een afbeelding.
