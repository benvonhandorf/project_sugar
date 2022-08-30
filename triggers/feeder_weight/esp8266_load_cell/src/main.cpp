#include <ESP8266WiFi.h>
#include <HX711_ADC.h>
#include <PubSubClient.h>

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

    initWiFi();
    initMQTT();
}

const uint32_t AVERAGING_WINDOW = 10;
float WINDOW[AVERAGING_WINDOW];
uint16_t AVG_READING = 0;
float AVERAGE = 0.0f;
char fmt[12];
uint8_t ignoredReadings = 0;
const float EXPECTED_TRIGGER_MIN = 1.5;
const float EXPECTED_TRIGGERS_MAX = 5;
bool initialized = false;

void loop() {
    if (LoadCell.update()) {
        float val = LoadCell.getData();

        float delta = AVERAGE - val;

        // In planned installation, increase in weight should be a more negative
        // number. delta should

        if (abs(delta) > 100) {
            // > 100g delta.  Something is suspect.
            ignoredReadings++;
        }

        // if (!initialized) {
            WINDOW[AVG_READING++] = val;
            ignoredReadings = 0;
        // } else if (abs(delta) <= 100 || ignoredReadings > 10) {
        //     if ((abs(delta) < EXPECTED_TRIGGERS_MAX &&
        //          abs(delta) > EXPECTED_TRIGGER_MIN)) {
        //         dtostrf(AVERAGE, 5, 3, fmt);

        //         Serial.print("Trigger reading: ");
        //         Serial.println(fmt);

        //         client.publish("/sensors/feeder/trigger", fmt);
        //     } else {
        //         WINDOW[AVG_READING++] = val;
        //         ignoredReadings = 0;
        //     }
        // } else {
        //     dtostrf(AVERAGE, 5, 3, fmt);

        //     Serial.print("Ignoring reading: ");
        //     Serial.println(fmt);
        // }

        if (AVG_READING >= AVERAGING_WINDOW) {
            // Recalculate the average and print it
            float avg_value = 0.0f;
            for (uint16_t i = 0; i < AVERAGING_WINDOW; i++) {
                avg_value += WINDOW[i];
            }

            avg_value /= AVERAGING_WINDOW;
            AVERAGE = avg_value;
            initialized = true;

            dtostrf(AVERAGE, 5, 3, fmt);

            Serial.print("Calculated average: ");
            Serial.println(fmt);

            AVG_READING = 0;

            client.publish("/sensors/feeder/average", fmt);
        }
    }

    delay(50);
}