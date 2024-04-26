#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <HX711_ADC.h>
#include <PubSubClient.h>

#define WATCHDOG_MS 30000

#if FM == 1

#define FEEDER_ID "1"
const char *HOSTNAME = "feedermonitor01";

float calibrationValue = -1257.94;

#elif FM == 2

#define FEEDER_ID "2"
const char *HOSTNAME = "feedermonitor02";

float calibrationValue = -970.83;

#elif FM == 3

#define FEEDER_ID "3"
const char *HOSTNAME = "feedermonitor03";

float calibrationValue = 906.79;

#endif

#ifndef FEEDER_ID
#define FEEDER_ID "999"
#endif

#define ROOT_TOPIC "sensors/feeders/"

const char *WILL_TOPIC = ROOT_TOPIC FEEDER_ID "/status";
const char *RESET_TOPIC = ROOT_TOPIC FEEDER_ID "/reset";
const char *BATCH_RAW_TOPIC = ROOT_TOPIC FEEDER_ID "/raw_batch";
const char *AVERAGE_TOPIC = ROOT_TOPIC FEEDER_ID "/average";
const char *TRIGGER_TOPIC = ROOT_TOPIC FEEDER_ID "/trigger";
const char *MESSAGE_TOPIC = ROOT_TOPIC FEEDER_ID "/message";
const char *RSSI_TOPIC = ROOT_TOPIC FEEDER_ID "/rssi_batch";
const char *CONTROL_TOPIC = ROOT_TOPIC FEEDER_ID "/control";
const char *WATCHDOG_TOPIC = ROOT_TOPIC FEEDER_ID "/watchdog";

bool debug_publish = false;

const char *mqtt_server = "littlerascal";
const char *MQTT_USER = "sensor_writer";
const char *MQTT_PASSWORD = "fln0eFi79yhK";

const int OTA_PORT = 8267;
const char *OTA_PASSWORD = "dlafjsdlk";

const long WATCHDOG_TIMEOUT_MS = 3600000;
long watchdog_timer = WATCHDOG_TIMEOUT_MS;

// HX711 constructor:
HX711_ADC LoadCell(HX711_dout, HX711_sck);

WiFiClient espClient;
PubSubClient client(espClient);

void println(const char *s) {
    Serial.println(s);
    if(debug_publish) {
        client.publish(MESSAGE_TOPIC, s);
    }
}

void println(const String &s) {
    Serial.println(s);
    if(debug_publish) {
        client.publish(MESSAGE_TOPIC, s.c_str());
    }
}

void print(const char *s) {
    Serial.print(s);
    if(debug_publish) {
        client.publish(MESSAGE_TOPIC, s);
    }
}

void halt() {
    while (1) {
        delay(1000);
    }
}

#if FM == 2
//FM2 is closer to the main wifi AP
const char *WIFI_AP_NAME = "Oblivion";
const char *WIFI_PASS = "t4unjath0mson";

#else

const char *WIFI_AP_NAME = "OblivionOD";
const char *WIFI_PASS = "outerdarkness1!";

#endif

uint32_t reset_reason = 0;
uint32_t reset_exception = 0;

void initWiFi_block() {
    WiFi.mode(WIFI_STA);
    
    WiFi.begin(WIFI_AP_NAME, WIFI_PASS);
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

    WiFi.setAutoReconnect(true);
}

void MQTT_reconnect() {
    uint32_t retries = 10;

    while (!client.connected()) {
        if(!WiFi.isConnected()) {
            println("Wifi Disconnected.  Attempting to reconnect.");

            initWiFi_block();
            delay(5000);
        }

        println("Attempting MQTT connection...");

        if (client.connect(HOSTNAME, MQTT_USER, MQTT_PASSWORD, WILL_TOPIC,
                           (uint8_t)1, true, "offline")) {
            client.publish(WILL_TOPIC, "online", true);
            client.publish(RESET_TOPIC, String(reset_reason).c_str(), true);
        } else {
            print("MQTT Attempt Failed, rc=");
            println(String(client.state()));

            delay(3000);
        }

        retries--;

        if(retries == 0) {
            ESP.restart();
        }
    }

    client.subscribe(CONTROL_TOPIC);
}

void MQTT_callback(char* topic, byte* payload, unsigned int length) {
    println("Message arrived [" + String(topic) + "] ");

    String msg = "";

    for (uint32_t i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    println(msg);

    if(strcmp(topic, CONTROL_TOPIC) == 0) {
        if(strcmp(msg.c_str(), "reset") == 0) {
            println("Resetting");
            ESP.restart();
        }
    } else if(strcmp(topic, WATCHDOG_TOPIC) == 0) {
        watchdog_timer = WATCHDOG_TIMEOUT_MS;

        println("Watchdog Reset");
    }
}

void initMQTT_block() {
    client.setServer(mqtt_server, 1883);
    client.setBufferSize(2048);
    client.setCallback(MQTT_callback);

    MQTT_reconnect();
}

void initOTA_block() {
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

const uint32_t AVERAGING_WINDOW_LENGTH = 5;
uint16_t averagedReadings = 0;
uint16_t readingCount = 0;
float calculatedAverage = 0.0f;

char fmt[12];
uint8_t triggeredReadings = 0;
const float EXPECTED_TRIGGER_MIN = 1.75;
const float EXPECTED_TRIGGER_MAX = 40.0;
const float TRIGGER_HYSTERISIS = 1.25;
const float TRIGGER_RESET_INTERVAL = 100;
bool initialized = false;
bool triggered = false;

const int RAW_BATCH_SIZE = 256;
uint32_t rawFirstSample = 0;
float rawReadings[2048];
uint16_t rawReadingsCount = 0;

const uint32_t RSSI_BATCH_SIZE = 256;
uint32_t rssiFirstSample = 0;
int8_t rssiReadings[2048];
uint16_t rssiReadingsCount = 0;

uint32_t LOOP_DELAY_MS = 0;




float readDataWithAveraging() {
    float value = 0.0f;
    averagedReadings = 0;

    while (averagedReadings < AVERAGING_WINDOW_LENGTH) {
        if (LoadCell.update()) {
            value += LoadCell.getData();

            averagedReadings++;

            ArduinoOTA.handle();
        }
    }

    value /= (float)AVERAGING_WINDOW_LENGTH;

    return value;
}

void readStartupData() {
    if (readingCount == 0) {
        while (readingCount < AVERAGING_WINDOW_LENGTH) {
            calculatedAverage += readDataWithAveraging();
            readingCount++;

            ArduinoOTA.handle();

            delay(5);
        }

        calculatedAverage /= (float)AVERAGING_WINDOW_LENGTH;
    }

    initialized = true;
    readingCount = 0;

    dtostrf(calculatedAverage, 5, 3, fmt);
    print("Startup Data Acquired: ");
    println(fmt);
}

void publishTrigger(bool triggered, float delta) {
    dtostrf(delta, 5, 3, fmt);

    String msg = "{ \"value\": " + String(triggered) +
                 ", \"delta\": " + String(delta) + "}";

    client.publish(TRIGGER_TOPIC, msg.c_str());
}

bool writeBatchFloat(const char *topic, float *readings, uint16_t count,
                     uint32_t durationMs) {
String msg = "{\"duration\":" + String(durationMs) + ",\"data\":[";


    for (uint16_t i = 0; i < count; i++) {
        dtostrf(readings[i], 5, 3, fmt);
        msg += fmt;

        if (i < count - 1) {
            msg += ",";
        }
    }
    msg += "]}";

    println(String(msg.length()));

    bool result = client.publish(topic, msg.c_str());

    return result;
}

bool writeBatchInt8(const char *topic, int8_t *readings, uint16_t count,
                    uint32_t durationMs) {

    String msg = "{\"duration\":" + String(durationMs) + ",\"data\":[";

    for (uint16_t i = 0; i < count; i++) {
        itoa(readings[i], fmt, 10);
        msg += fmt;

        if (i < count - 1) {
            msg += ",";
        }
    }
    msg += "]}";

    // println(msg);

    bool result = client.publish(topic, msg.c_str());

    return result;
}

void setup() {
    struct rst_info *rst_info = system_get_rst_info();

    reset_reason = rst_info->reason;
    reset_exception = rst_info->exccause;

    Serial.begin(115200);
    delay(10);

    Serial.printf("Reset reason: %d\n", rst_info->reason);

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

    ESP.wdtEnable(WATCHDOG_MS);

    initWiFi_block();
    initOTA_block();
    initMQTT_block();

    readStartupData();

    publishTrigger(false, 0.0f);
}

long last_ms = 0;

void loop() {
    if (!client.connected()) {
        MQTT_reconnect();
    }

    if(last_ms == 0) {
        last_ms = millis();
    }

    watchdog_timer -= millis() - last_ms;
    last_ms = millis();

    if(watchdog_timer < 0) {
        println("Watchdog Reset");
        
        client.flush();

        ESP.reset();
    }

    bool success = false;
    float val = readDataWithAveraging();
    readingCount++;

    float delta = abs(calculatedAverage - val);

    rawReadings[rawReadingsCount++] = val;

    if(rawFirstSample == 0) {
        rawFirstSample = millis();
    }

    if (rawReadingsCount > RAW_BATCH_SIZE) {
        println("Writing batch to raw topic");

        uint32_t duration = millis() - rawFirstSample;

        client.loop();

        success =
            writeBatchFloat(BATCH_RAW_TOPIC, rawReadings, rawReadingsCount, duration);

        if (success || rawReadingsCount > 1024) {
            println("Raw batch complete.");
            rawReadingsCount = 0;
            rawFirstSample = 0;
        } else {
            client.loop(); //Added to try and ensure that the client.state() is accurate
            debug_publish = true;
            println("Raw batch failed.");
            println(String(client.state()));
            debug_publish = false;
        }
    }

    if (delta > EXPECTED_TRIGGER_MIN && delta < EXPECTED_TRIGGER_MAX &&
        triggeredReadings < TRIGGER_RESET_INTERVAL && initialized) {
        // Triggered
        if (!triggered) {
            publishTrigger(true, delta);

            triggered = true;

            print("Triggered.");
        }

        triggeredReadings++;
    } else if (delta > TRIGGER_HYSTERISIS) {
        // We haven't dropped to within 1g of our starting weight
        // Assume the hummingbird is still present and this is noise in the data
        triggeredReadings++;
    } else {
        if (triggered) {
            publishTrigger(false, delta);

            triggered = false;
            triggeredReadings = 0;

            print("Trigger Cleared.");
            println(fmt);
        }

        calculatedAverage = (calculatedAverage * 0.9) + (val * 0.1);

        if (readingCount % 20 == 0) {
            dtostrf(calculatedAverage, 5, 3, fmt);

            print("Calculated average: ");
            Serial.println(fmt);

            success = client.publish(AVERAGE_TOPIC, fmt) || success;
        }
    }

    int8_t rssi = WiFi.RSSI();

    rssiReadings[rssiReadingsCount++] = rssi;

    if(rssiFirstSample == 0) {
        rssiFirstSample = millis();
    }

    if (rssiReadingsCount > RSSI_BATCH_SIZE) {
        println("Writing batch to rssi topic");

        uint32_t duration = millis() - rssiFirstSample;

        success =
            writeBatchInt8(RSSI_TOPIC, rssiReadings, rssiReadingsCount, duration);

        if (success || rssiReadingsCount > 1024) {
            println("RSSI batch complete.");
            rssiReadingsCount = 0;
            rssiFirstSample = 0;
        }
    }

    ArduinoOTA.handle();

    delay(LOOP_DELAY_MS);
}