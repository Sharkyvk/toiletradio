# Toiletradio

Internetradio met hotel-gevoel voor het toilet.

- **Plafondspeler** (ESP32-S3, in de lamp): speelt 5 internetzenders, leest licht en aanwezigheid (radar), bewaakt de stille uren (standaard 23:30–06:00), fade-in/fade-uit.
- **Muurpaneel** (Waveshare ESP32-S3-Touch-LCD-1.69, in de inbouwdoos): touchscreen met klok, 5 presets en mute. Gebruikers kunnen hier alleen een zender kiezen, stoppen en dempen.
- **Beheer** (alleen beheerder): via Bluetooth met pincode, vanaf de beheerpagina. Reserve: wifi-dashboard op `http://toiletradio.local`.

## Inhoud

| Map | Wat |
|---|---|
| `docs/index.html` | Beheerpagina (Bluetooth), wordt gepubliceerd via GitHub Pages |
| `firmware/toiletradio_plafond/` | Firmware plafondspeler |
| `firmware/toiletradio_paneel/` | Firmware muurpaneel |

## Beheerpagina online zetten (GitHub Pages)

1. Maak op GitHub een nieuwe **openbare** repository, bijv. `toiletradio`.
2. Upload de inhoud van deze map (of `git push`).
3. Ga naar **Settings → Pages**.
4. Bij *Source*: kies **Deploy from a branch**, branch **main**, map **/docs** → **Save**.
5. Na een minuut staat de pagina op `https://<jouw-gebruikersnaam>.github.io/toiletradio/`.

Openen:
- **Android:** in Chrome.
- **iPhone:** in de gratis app **Bluefy** (Safari ondersteunt geen Bluetooth).

Tik op *Verbinden met radio*, kies **Toiletradio** en voer de pincode in.

> De pagina bevat geen wachtwoorden. De beveiliging zit in de pincode op de radio.
> Wijzig direct na installatie de standaardcodes: Bluetooth-pincode `123456` en dashboardwachtwoord `toilet`.

## Firmware bouwen (Arduino IDE)

Board-instellingen (beide): *ESP32S3 Dev Module*, Flash 16MB, Partition *16M Flash (3MB APP/9.9MB FATFS)*, PSRAM *OPI PSRAM*, *USB CDC On Boot: Enabled*. Arduino-ESP32 core 3.x.

Libraries:

| Library | Plafond | Paneel |
|---|:-:|:-:|
| ESP32-audioI2S (schreibfaul1, v3.x, GitHub-ZIP) | ✓ | |
| WiFiManager (tzapu) | ✓ | ✓ |
| NimBLE-Arduino (h2zero, v2.x) | ✓ | |
| ArduinoJson (v7) | ✓ | |
| GFX Library for Arduino (moononournation) | | ✓ |
| U8g2 (olikraus) | | ✓ |

## Eerste start

1. **Plafondspeler:** wifi instellen via de beheerpagina (Bluetooth) of via het wifi-netwerk `Toiletradio-Setup`.
2. **Muurpaneel:** verbinden met wifi-netwerk `Toiletpaneel-Setup` en hetzelfde thuisnetwerk kiezen. Het paneel vindt de speler daarna zelf.
3. Geef beide in de router een vast IP-adres (DHCP-reservering).

## Status

Versie 2.1 – nog niet op hardware getest.
