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
const char *RESET_TOPIC = "/sensors/feeders/3/reset";
const char *RAW_TOPIC = "/sensors/feeders/1/raw";
const char *AVERAGE_TOPIC = "/sensors/feeders/1/average";
const char *TRIGGER_TOPIC = "/sensors/feeders/1/trigger";
const char *MESSAGE_TOPIC = "/sensors/feeders/1/message";
const char *RSSI_TOPIC = "/sensors/feeders/1/rssi";

#elif FM==2

float calibrationValue = 986.94;

const char *HOSTNAME = "feedermonitor02";
const char *WILL_TOPIC = "/sensors/feeders/2/status";
const char *RESET_TOPIC = "/sensors/feeders/2/reset";
const char *RAW_TOPIC = "/sensors/feeders/2/raw";
const char *AVERAGE_TOPIC = "/sensors/feeders/2/average";
const char *TRIGGER_TOPIC = "/sensors/feeders/2/trigger";
const char *MESSAGE_TOPIC = "/sensors/feeders/2/message";
const char *RSSI_TOPIC = "/sensors/feeders/2/rssi";

#elif FM==3

float calibrationValue = 906.79;

const char *HOSTNAME = "feedermonitor03";
const char *WILL_TOPIC = "/sensors/feeders/3/status";
const char *RESET_TOPIC = "/sensors/feeders/3/reset";
const char *RAW_TOPIC = "/sensors/feeders/3/raw";
const char *AVERAGE_TOPIC = "/sensors/feeders/3/average";
const char *TRIGGER_TOPIC = "/sensors/feeders/3/trigger";
const char *MESSAGE_TOPIC = "/sensors/feeders/3/message";
const char *RSSI_TOPIC = "/sensors/feeders/3/rssi";

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
    client.publish(MESSAGE_TOPIC, s, true);
}

void println(const String &s) {
    Serial.println(s);
    client.publish(MESSAGE_TOPIC, s.c_str(), true);
}

void print(const char *s) {
    Serial.print(s);
    client.publish(MESSAGE_TOPIC, s, true);
}

void halt() {
    while (1) {
        delay(1000);
    }
}

const char *WIFI_AP_NAME = "OBLIVION";
const char *WIFI_PASS = "t4unjath0mson";

uint32_t reset_reason = 0;
uint32_t reset_exception = 0;

void initWiFi_block() {
    WiFi.mode(WIFI_STA);
    // WiFi.begin("FakeSun", "T4vyt3FYWCs9YjChzuh7DeQV");
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
}

void initMQTT_block() {
    client.setServer(mqtt_server, 1883);
    client.connect(HOSTNAME, MQTT_USER, MQTT_PASSWORD, WILL_TOPIC,
                   (uint8_t)1, true, "offline");

    if (client.state() != MQTT_CONNECTED) {
        print("MQTT Failed: ");
        Serial.println(client.state());

        halt();
    }

    Serial.println("MQTT Connected.");

    client.publish(WILL_TOPIC, "online", true);
    client.publish(RESET_TOPIC, String(reset_reason).c_str(), true);
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

const uint32_t AVERAGING_WINDOW_LENGTH = 20;
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

float readDataWithAveraging() {
    float value = 0.0f;
    averagedReadings = 0;

    while(averagedReadings < AVERAGING_WINDOW_LENGTH)  {
        if (LoadCell.update()) {
            value += LoadCell.getData();

            averagedReadings++;

            delay(50);
        }
    }

    value /= (float) AVERAGING_WINDOW_LENGTH;

    return value;
}

void readStartupData() {
    if (readingCount == 0) {
        while (readingCount < AVERAGING_WINDOW_LENGTH) {
            calculatedAverage += readDataWithAveraging();
            readingCount++;

            delay(50);
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

    String msg = "{ \"value\": " + String(triggered) + ", \"delta\": " +
                 String(delta) + "}";

    client.publish(TRIGGER_TOPIC, msg.c_str());
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

void loop() {
    bool success = false;
    float val = readDataWithAveraging();
    readingCount++;

    float delta = abs(calculatedAverage - val);

    dtostrf(val, 5, 3, fmt);
    success = client.publish(RAW_TOPIC, fmt) || success;

    println(fmt);

    if (delta > EXPECTED_TRIGGER_MIN &&
        delta < EXPECTED_TRIGGER_MAX &&
        triggeredReadings < TRIGGER_RESET_INTERVAL && initialized) {
        // Triggered
        if (!triggered) {
            publishTrigger(true, delta);

            triggered = true;

            print("Triggered.");
        }

        triggeredReadings++;
    } else if(delta > TRIGGER_HYSTERISIS) {
        //We haven't dropped to within 1g of our starting weight
        //Assume the hummingbird is still present and this is noise in the data
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

    itoa(rssi, fmt, 10);

    if(WiFi.isConnected()) {
        success = client.publish(RSSI_TOPIC, fmt) || success;

        if(success ) {
            ESP.wdtFeed();
        } else {
            println("No successful transmissions.");
            ESP.restart();
        }
    }  else {
        println("WiFi not connected.");
        ESP.restart();
    }
    
    ArduinoOTA.handle();

    delay(100);
}