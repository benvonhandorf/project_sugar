#include <ESP8266WiFi.h>
#include <HX711_ADC.h>
#include <PubSubClient.h>

#define WATCHDOG_MS 30000

// pins:
// const int HX711_dout = 25;  // mcu > HX711 dout pin
// const int HX711_sck = 26;   // mcu > HX711 sck pin

const int HX711_dout = 13;  // mcu > HX711 dout pin
const int HX711_sck = 15;   // mcu > HX711 sck pin

// HX711 constructor:
HX711_ADC LoadCell(HX711_dout, HX711_sck);

void halt() {
    while (1) {
        delay(1000);
    }
}

void initWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin("FakeSun", "T4vyt3FYWCs9YjChzuh7DeQV");
    Serial.println("Conencting to WiFI");
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print('.');
        delay(500);
    }

    Serial.println();
    Serial.println(WiFi.localIP());
}

WiFiClient espClient;
PubSubClient client(espClient);

const char *mqtt_server = "littlerascal";
const char *CLIENT_NAME = "feeder";
const char *MQTT_USER = "sensor_writer";
const char *MQTT_PASSWORD = "fln0eFi79yhK";
const char *WILL_TOPIC = "/sensors/feeder/status";

void initMQTT() {
    client.setServer(mqtt_server, 1883);
    client.connect(CLIENT_NAME, MQTT_USER, MQTT_PASSWORD, WILL_TOPIC,
                   (uint8_t)1, true, "offline");

    if (client.state() != MQTT_CONNECTED) {
        Serial.print("MQTT Failed: ");
        Serial.println(client.state());

        halt();
    }

    Serial.println("MQTT Connected.");

    client.publish(WILL_TOPIC, "online");
}

void setup() {
    Serial.begin(115200);
    delay(10);

    Serial.println("setup begin");

    LoadCell.begin();

    float calibrationValue = -1088.26;

    LoadCell.start(2000, true);

    if (LoadCell.getTareTimeoutFlag()) {
        Serial.println("Timeout, check MCU>HX711 wiring and pin designations");

        halt();
    } else {
        LoadCell.setCalFactor(
            calibrationValue);  // set calibration value (float)
        Serial.println("LoadCell is complete");
    }

    ESP.wdtEnable(WATCHDOG_MS);

    initWiFi();
    initMQTT();
}

const uint32_t AVERAGING_WINDOW_LENGTH = 10;
float WINDOW[AVERAGING_WINDOW_LENGTH];
uint16_t averagingReadingLocation = 0;
float calculatedAverage = 0.0f;
char fmt[12];
uint8_t triggeredReadings = 0;
const float EXPECTED_TRIGGER_MIN = 1.5;
const float EXPECTED_TRIGGER_MAX = 10.0;
const float TRIGGER_RESET_INTERVAL = 100;
bool initialized = false;
bool triggered = false;

float readData() {
    float readWindow[AVERAGING_WINDOW_LENGTH];
    uint16_t readLocation = 0;

    while(readLocation < AVERAGING_WINDOW_LENGTH) {
        if(LoadCell.update()) {
            readWindow[readLocation++] = LoadCell.getData();
        }
    }

    float avg_value = 0.0f;

    for (uint16_t i = 0; i < AVERAGING_WINDOW_LENGTH; i++) {
        avg_value += readWindow[i];
    }

    avg_value /= (float) AVERAGING_WINDOW_LENGTH;

    return avg_value;
}

void loop() {
    float val = readData();

    float delta = calculatedAverage - val;

    dtostrf(val, 5, 3, fmt);
    client.publish("/sensors/feeder/raw", fmt);

    // In planned installation, increase in weight should be a more negative
    // number. delta should

    if (abs(delta) > EXPECTED_TRIGGER_MIN 
            && abs(delta) < EXPECTED_TRIGGER_MAX
            && triggeredReadings < TRIGGER_RESET_INTERVAL
            && initialized) {
        //Triggered
        if(!triggered) {
            client.publish("/sensors/feeder/trigger", "true");
            triggered = true;

            Serial.print("Triggered: ");
            Serial.println(fmt);
        }

        triggeredReadings++;
    } else {
        if(triggered) {
            client.publish("/sensors/feeder/trigger", "false");
            triggered = false;
            triggeredReadings = 0;

            Serial.print("Trigger Cleared: ");
            Serial.println(fmt);
        }
        
        WINDOW[averagingReadingLocation++] = val;

        if (averagingReadingLocation >= AVERAGING_WINDOW_LENGTH) {
            initialized = true;
            averagingReadingLocation = 0;
        }

        if(initialized) {
            float avg_value = 0.0f;
            for (uint16_t i = 0; i < AVERAGING_WINDOW_LENGTH; i++) {
                avg_value += WINDOW[i];
            }

            avg_value /= AVERAGING_WINDOW_LENGTH;
            calculatedAverage = avg_value;

            if(averagingReadingLocation == 0) {
                dtostrf(calculatedAverage, 5, 3, fmt);

                Serial.print("Calculated average: ");
                Serial.println(fmt);

                client.publish("/sensors/feeder/average", fmt);
            }
        }

    }

    ESP.wdtFeed();

    delay(100);
}