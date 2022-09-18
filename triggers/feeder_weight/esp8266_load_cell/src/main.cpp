#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <HX711_ADC.h>
#include <PubSubClient.h>

#define WATCHDOG_MS 30000

#if FM==1
float calibrationValue = -970.83;

const char *HOSTNAME = "feedermonitor01";
const char *WILL_TOPIC = "/sensors/feeders/1/status";
const char *RAW_TOPIC = "/sensors/feeders/1/raw";
const char *AVERAGE_TOPIC = "/sensors/feeders/1/average";
const char *TRIGGER_TOPIC = "/sensors/feeders/1/trigger";
const char *MESSAGE_TOPIC = "/sensors/feeders/1/message";

#elif FM==2
float calibrationValue = 986.94;

const char *HOSTNAME = "feedermonitor02";
const char *WILL_TOPIC = "/sensors/feeders/2/status";
const char *RAW_TOPIC = "/sensors/feeders/2/raw";
const char *AVERAGE_TOPIC = "/sensors/feeders/2/average";
const char *TRIGGER_TOPIC = "/sensors/feeders/2/trigger";
const char *MESSAGE_TOPIC = "/sensors/feeders/2/message";
#elif FM==3
float calibrationValue = 906.79;

const char *HOSTNAME = "feedermonitor03";
const char *WILL_TOPIC = "/sensors/feeders/3/status";
const char *RAW_TOPIC = "/sensors/feeders/3/raw";
const char *AVERAGE_TOPIC = "/sensors/feeders/3/average";
const char *TRIGGER_TOPIC = "/sensors/feeders/3/trigger";
const char *MESSAGE_TOPIC = "/sensors/feeders/3/message";
#endif

const char *mqtt_server = "littlerascal";
const char *MQTT_USER = "sensor_writer";
const char *MQTT_PASSWORD = "fln0eFi79yhK";

const int OTA_PORT = 8267;
const char *OTA_PASSWORD = "dlafjsdlk";

// HX711 constructor:
HX711_ADC LoadCell(HX711_dout, HX711_sck);

WiFiClient espClient;
PubSubClient client(espClient);

void println(const char *s) {
    Serial.println(s);
    client.publish(MESSAGE_TOPIC, s);
}

void println(const String &s) {
    Serial.println(s);
    client.publish(MESSAGE_TOPIC, s.c_str());
}

void print(const char *s) {
    Serial.print(s);
    client.publish(MESSAGE_TOPIC, s);
}

void halt() {
    while (1) {
        delay(1000);
    }
}

void initWiFi() {
    WiFi.mode(WIFI_STA);
    // WiFi.begin("FakeSun", "T4vyt3FYWCs9YjChzuh7DeQV");
    WiFi.begin("OBLIVION", "t4unjath0mson");
    Serial.println("Conencting to WiFI");
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print('.');
        delay(500);
    }

    Serial.println();
    Serial.println(WiFi.localIP());

    if (!MDNS.begin(HOSTNAME)) {
        Serial.println("Error setting up MDNS responder!");
    }
}

void initMQTT() {
    client.setServer(mqtt_server, 1883);
    client.connect(HOSTNAME, MQTT_USER, MQTT_PASSWORD, WILL_TOPIC,
                   (uint8_t)1, true, "offline");

    if (client.state() != MQTT_CONNECTED) {
        print("MQTT Failed: ");
        Serial.println(client.state());

        halt();
    }

    Serial.println("MQTT Connected.");

    client.publish(WILL_TOPIC, "online");
}

void initOTA() {
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.setHostname(HOSTNAME);

    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else {  // U_FS
            type = "filesystem";
        }

        // NOTE: if updating FS this would be the place to unmount FS using
        // FS.end()
        Serial.println("Start updating " + type);
    });
    ArduinoOTA.onEnd([]() { Serial.println("\nEnd"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) {
            println("Auth Failed");
        } else if (error == OTA_BEGIN_ERROR) {
            println("Begin Failed");
        } else if (error == OTA_CONNECT_ERROR) {
            println("Connect Failed");
        } else if (error == OTA_RECEIVE_ERROR) {
            println("Receive Failed");
        } else if (error == OTA_END_ERROR) {
            println("End Failed");
        }
    });
    ArduinoOTA.begin();
}

void setup() {
    Serial.begin(115200);
    delay(10);

    Serial.println("setup begin");

    LoadCell.begin();

    LoadCell.start(2000, true);

    if (LoadCell.getTareTimeoutFlag()) {
        Serial.println("Timeout, check MCU>HX711 wiring and pin designations");

        halt();
    } else {
        LoadCell.setCalFactor(
            calibrationValue);  // set calibration value (float)
        Serial.println("LoadCell is complete");
    }

    // ESP.wdtEnable(WATCHDOG_MS);

    initWiFi();
    initOTA();
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

    while (readLocation < AVERAGING_WINDOW_LENGTH) {
        if (LoadCell.update()) {
            readWindow[readLocation++] = LoadCell.getData();
        }
    }

    float avg_value = 0.0f;

    for (uint16_t i = 0; i < AVERAGING_WINDOW_LENGTH; i++) {
        avg_value += readWindow[i];
    }

    avg_value /= (float)AVERAGING_WINDOW_LENGTH;

    return avg_value;
}

void loop() {
    float val = readData();

    float delta = calculatedAverage - val;

    dtostrf(val, 5, 3, fmt);
    client.publish(RAW_TOPIC, fmt);

    // In planned installation, increase in weight should be a more negative
    // number. delta should

    if (abs(delta) > EXPECTED_TRIGGER_MIN &&
        abs(delta) < EXPECTED_TRIGGER_MAX &&
        triggeredReadings < TRIGGER_RESET_INTERVAL && initialized) {
        // Triggered
        if (!triggered) {
            client.publish(TRIGGER_TOPIC, "true");
            triggered = true;

            print("Triggered: ");
            println(fmt);
        }

        triggeredReadings++;
    } else {
        if (triggered) {
            client.publish(TRIGGER_TOPIC, "false");
            triggered = false;
            triggeredReadings = 0;

            print("Trigger Cleared: ");
            println(fmt);
        }

        WINDOW[averagingReadingLocation++] = val;

        if (averagingReadingLocation >= AVERAGING_WINDOW_LENGTH) {
            initialized = true;
            averagingReadingLocation = 0;
        }

        if (initialized) {
            float avg_value = 0.0f;
            for (uint16_t i = 0; i < AVERAGING_WINDOW_LENGTH; i++) {
                avg_value += WINDOW[i];
            }

            avg_value /= AVERAGING_WINDOW_LENGTH;
            calculatedAverage = avg_value;

            if (averagingReadingLocation == 0) {
                dtostrf(calculatedAverage, 5, 3, fmt);

                print("Calculated average: ");
                Serial.println(fmt);

                client.publish(AVERAGE_TOPIC, fmt);
            }
        }
    }

    ESP.wdtFeed();

    ArduinoOTA.handle();

    delay(100);
}