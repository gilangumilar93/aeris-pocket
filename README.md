# AERIS Pocket

ESP32-S3 pocket air-quality and UV monitor. SGP41, VEML6075, 1.3 inch SH1106, MQTT discovery for Home Assistant.

AQI* is a comfort figure mapped from the Sensirion VOC index. It is not an EPA AQI. The SGP41 does not see particles or ozone.

## Display fix

The air page drew the comfort band at a fixed x of 88. In `u8g2_font_6x12_tr`, "Elevated" is 48 px wide, so the last letters ran off the 128 px glass. `drawAir()` now measures the band with `getStrWidth` and right-aligns it. The NOx caption is shortened to `1 = clean baseline` (the previous line was 156 px). The system page draws the IP at y = 63, the last visible row, instead of y = 64 where it was clipped.

## Flash

1. Copy `firmware/secrets.h` and fill in the LAN SSID, password, and MQTT broker. The sketch speaks plaintext MQTT. Do not commit real passwords.
2. Board: ESP32S3 Dev Module, USB CDC On Boot Enabled, Flash Size 8 MB, PSRAM Disabled, Arduino-ESP32 core 3.x.
3. Library Manager: Sensirion I2C SGP41, Sensirion Gas Index Algorithm, SparkFun VEML6075 Arduino Library, U8g2, PubSubClient, ArduinoJson 7.
4. Open `firmware/aeris_pocket.ino`. `secrets.h` sits next to it.

Keep sampling at 1 Hz. Slowing the Sensirion gas-index algorithm makes the index meaningless.

State topic: `aeris/pocket/<mac>/state`. Availability uses a last-will of `offline`. Discovery is published on connect. `firmware/homeassistant.yaml` is the manual backup if discovery is off. Replace `aabbccddeeff` with the id on the system page.

## Enclosure

`enclosure/aeris_enclosure.py` is a FreeCAD 1.0 script.

    freecadcmd aeris_enclosure.py

It writes STEP and STL for the bottom and the lid next to the script. Leave the UV hole open. PETG and PLA block UVA and UVB. Acetate belongs only in the OLED rebate.
