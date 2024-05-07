/*
   -------------------------------------------------------------------------------------
   HX711_ADC
   Arduino library for HX711 24-Bit Analog-to-Digital Converter for Weight Scales
   Olav Kallhovd sept2017
   -------------------------------------------------------------------------------------
*/

/*
   This example file shows how to calibrate the load cell and optionally store the calibration
   value in EEPROM, and also how to change the value manually.
   The result value can then later be included in your project sketch or fetched from EEPROM.

   To implement calibration in your project sketch the simplified procedure is as follow:
       LoadCell.tare();
       //place known mass
       LoadCell.refreshDataSet();
       float newCalibrationValue = LoadCell.getNewCalibration(known_mass);
*/

#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include "ESPTelnet.h"

#include <HX711_ADC.h>
#if defined(ESP8266) || defined(ESP32) || defined(AVR)
#include <EEPROM.h>
#endif

#if FM == 1

#define FEEDER_ID "1"
const char *HOSTNAME = "feedermonitor01";

#elif FM == 2

#define FEEDER_ID "2"
const char *HOSTNAME = "feedermonitor02";

#elif FM == 3

#define FEEDER_ID "3"
const char *HOSTNAME = "feedermonitor03";

#endif

ESPTelnet telnet;

// const char *WIFI_AP_NAME = "Oblivion";
// const char *WIFI_PASS = "t4unjath0mson";

const char *WIFI_AP_NAME = "OblivionOD";
const char *WIFI_PASS = "outerdarkness1!";

const int OTA_PORT = 8267;
const char *OTA_PASSWORD = "dlafjsdlk";

uint32_t reset_reason = 0;
uint32_t reset_exception = 0;

String input;

void print(const String str)
{
  if (telnet.isConnected())
  {
    telnet.print(str);
  }
  Serial.print(str);
}

void println(const String str)
{
  if (telnet.isConnected())
  {
    telnet.println(str);
  }
  Serial.println(str);
}

void println(int i)
{
  String result(i);
  println(result);
}

void print(float f)
{
  String result(f, 6);
  print(result);
}

void println()
{
  println("");
}

String serialIn;

void serviceInputs()
{
  ArduinoOTA.handle();

  telnet.loop();

  if (Serial.available() > 0)
  {
    char inByte = Serial.read();
    if (inByte == '\n')
    {
      input = serialIn;
      serialIn = "";
    }
    else
    {
      serialIn = serialIn + inByte;
    }
  }
}

float readFloat()
{
  serviceInputs();

  if (input.length() > 0)
  {
    float result = input.toFloat();

    if (result != 0.0F)
    {

      input = "";

      return result;
    }
  }

  return NAN;
}

char readChar()
{
  serviceInputs();

  if (input.length() > 0)
  {
    char result = input[0];

    input = "";

    return result;
  }
  else
  {
    return 0;
  }
}

// (optional) callback functions for telnet events
void onTelnetConnect(String ip)
{
  print("- Telnet: ");
  print(ip);
  println(" connected");
  println("\nWelcome " + telnet.getIP());
  println("(Use ^] + q  to disconnect.)");
}

void onTelnetDisconnect(String ip)
{
  print("- Telnet: ");
  print(ip);
  println(" disconnected");
  println();
}

void onTelnetReconnect(String ip)
{
  print("- Telnet: ");
  print(ip);
  println(" reconnected");
}

void onTelnetConnectionAttempt(String ip)
{
  print("- Telnet: ");
  print(ip);
  println(" tried to connected");
}

void initTelnet()
{
  // passing on functions for various telnet events
  telnet.onConnect(onTelnetConnect);
  telnet.onConnectionAttempt(onTelnetConnectionAttempt);
  telnet.onReconnect(onTelnetReconnect);
  telnet.onDisconnect(onTelnetDisconnect);

  // passing a lambda function
  telnet.onInputReceived([](String str)
                         {
    // checks for a certain command
    input = str;
   });

  println("- Telnet: ");

  if (telnet.begin())
  {
    println("running");
  }
  else
  {
    println("error.");
    println("Will reboot...");
  }
}

void initWiFi_block()
{
  WiFi.mode(WIFI_STA);

  WiFi.begin(WIFI_AP_NAME, WIFI_PASS);
  println("Conencting to WiFI");
  while (WiFi.status() != WL_CONNECTED)
  {
    print('.');
    delay(200);
  }

  println();
  println(WiFi.localIP().toString());

  if (!MDNS.begin(HOSTNAME))
  {
    println("Error setting up MDNS responder!");
  }

  WiFi.setAutoReconnect(true);
}

void initOTA_block()
{
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setHostname(HOSTNAME);

  ArduinoOTA.onStart([]()
                     {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else {  // U_FS
            type = "filesystem";
        }

        // NOTE: if updating FS this would be the place to unmount FS using
        // FS.end()
        println("Start updating " + type); });
  ArduinoOTA.onEnd([]()
                   { println("\nEnd"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { printf("Progress: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
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
        } });
  ArduinoOTA.begin();
}

// HX711 constructor:
HX711_ADC LoadCell(HX711_dout, HX711_sck);

const int calVal_eepromAdress = 0;
unsigned long t = 0;

void calibrate()
{
  println("***");
  println("Start calibration:");
  println("Place the load cell an a level stable surface.");
  println("Remove any load applied to the load cell.");
  println("Send 't' from serial monitor to set the tare offset.");

  boolean _resume = false;
  while (_resume == false)
  {
    LoadCell.update();

    char inByte = readChar();
    if (inByte == 't')
    {
      println("Beginning Tare");
      LoadCell.tareNoDelay();

      while (LoadCell.getTareStatus() == false)
      {
        LoadCell.update();
        serviceInputs();
        delay(10);
      }

      println("Tare complete");
      _resume = true;
    }
  }

  println("Now, place your known mass on the loadcell.");
  println("Then send the weight of this mass (i.e. 100.0) from serial monitor.");

  float known_mass = 0;
  _resume = false;
  while (_resume == false)
  {
    LoadCell.update();

    known_mass = readFloat();
    if (known_mass != 0 && known_mass < 2000000000)
    {
      print("Known mass is: ");
      println(known_mass);
      _resume = true;
    }
  }

  LoadCell.refreshDataSet();                                          // refresh the dataset to be sure that the known mass is measured correct
  float newCalibrationValue = LoadCell.getNewCalibration(known_mass); // get the new calibration value

  print("New calibration value has been set to: ");
  print(newCalibrationValue);
  println(", use this as calibration value (calFactor) in your project sketch.");

  println("End calibration");
  println("***");
  println("To re-calibrate, send 'r' from serial monitor.");
  println("For manual edit of the calibration value, send 'c' from serial monitor.");
  println("***");
}

void changeSavedCalFactor()
{
  float oldCalibrationValue = LoadCell.getCalFactor();
  boolean _resume = false;
  println("***");
  print("Current value is: ");
  println(oldCalibrationValue);
  println("Now, send the new value from serial monitor, i.e. 696.0");
  float newCalibrationValue;
  while (_resume == false)
  {
    newCalibrationValue = readFloat();
    if (newCalibrationValue != 0 && newCalibrationValue != NAN)
    {
      print("New calibration value is: ");
      println(newCalibrationValue);
      LoadCell.setCalFactor(newCalibrationValue);
      _resume = true;
    }
  }
  _resume = false;
  print("Save this value to EEPROM adress ");
  print(calVal_eepromAdress);
  println("? y/n");
  while (_resume == false)
  {
    char inByte = readChar();
    if (inByte == 'y')
    {
#if defined(ESP8266) || defined(ESP32)
      EEPROM.begin(512);
#endif
      EEPROM.put(calVal_eepromAdress, newCalibrationValue);
#if defined(ESP8266) || defined(ESP32)
      EEPROM.commit();
#endif
      EEPROM.get(calVal_eepromAdress, newCalibrationValue);
      print("Value ");
      print(newCalibrationValue);
      print(" saved to EEPROM address: ");
      println(calVal_eepromAdress);
      _resume = true;
    }
    else if (inByte == 'n')
    {
      println("Value not saved to EEPROM");
      _resume = true;
    }
  }
  println("End change calibration value");
  println("***");
}

void setup()
{
  Serial.begin(115200);
  delay(10);
  println();
  println("Starting...");

  initWiFi_block();

  initOTA_block();

  telnet.onConnect(onTelnetConnect);
  telnet.onConnectionAttempt(onTelnetConnectionAttempt);
  telnet.onReconnect(onTelnetReconnect);
  telnet.onDisconnect(onTelnetDisconnect);

  initTelnet();

  LoadCell.begin();
  // LoadCell.setReverseOutput(); //uncomment to turn a negative output value to positive
  unsigned long stabilizingtime = 2000; // preciscion right after power-up can be improved by adding a few seconds of stabilizing time
  boolean _tare = true;                 // set this to false if you don't want tare to be performed in the next step
  LoadCell.start(stabilizingtime, _tare);
  if (LoadCell.getTareTimeoutFlag() || LoadCell.getSignalTimeoutFlag())
  {
    println("Timeout, check MCU>HX711 wiring and pin designations");
    while (1)
      ;
  }
  else
  {
    LoadCell.setCalFactor(1.0); // user set calibration value (float), initial value 1.0 may be used for this sketch
    println("Startup is complete");
  }
  while (!LoadCell.update())
    ;
  // calibrate(); //start calibration procedure
}

void loop()
{
  static boolean newDataReady = 0;
  const int serialPrintInterval = 0; // increase value to slow down serial print activity

  // check for new data/start next conversion:
  if (LoadCell.update())
    newDataReady = true;

  // get smoothed value from the dataset:
  if (newDataReady)
  {
    if (millis() > t + serialPrintInterval)
    {
      float i = LoadCell.getData();
      print("Load_cell output val: ");
      println(i);
      newDataReady = 0;
      t = millis();
    }
  }

  char inputChar = readChar();

  if (inputChar)
  {
    if (inputChar == 't')
    {
      println("Beginning Tare");

      LoadCell.tareNoDelay(); // tare
    }
    else if (inputChar == 'r')
    {
      calibrate(); // calibrate
    }
    else if (inputChar == 'c')
    {
      changeSavedCalFactor(); // edit calibration value manually
    }
  }

  // check if last tare operation is complete
  if (LoadCell.getTareStatus() == true)
  {
    println("Tare complete");
  }
}
