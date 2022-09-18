## New Monitor Bring Up

Approximate 1.5 hrs to bring up a new feeder ignoring print time for enclosure.

- Hardware build
  - Print monitor enclosure
  - Install load cell
  - Wire HX711 to load cell
  - Build ESP8266 module
  - Build power supply
  - Wire HX711 to ESP8266 and power supply through channels in enclosure
- Calibration
  - Install hooks and load bar
  - Short GPIO0 to GND to install over serial port
  - Wire up TX and RX to ESP8266
  - Power ESP8266
  - Install calibration sketch
  - Tare
  - Weigh known weight.  Default calibration weight is 140.6g
  - Record calibration value, do not write to EEPROM
- Installation
  - Define feeder number
  - Create feeder definition section
    - Hostname
    - MQTT Topics
    - Calibration value from load cell
  - Change define in platformio.ini for feeder ID
  - Short GPIO0 to GND to install over serial port
  - Wire up TX and RX to ESP8266
  - Power ESP8266
  - Upload to ESP8266
  - Further updates can be done via OTA


## Troubleshooting

- Conflicting MQTT Client names cause earlier clients to be disconnected