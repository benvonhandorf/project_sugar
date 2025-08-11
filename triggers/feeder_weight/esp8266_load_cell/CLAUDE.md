# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP8266-based feeder weight monitoring system using HX711 load cells. The system monitors hummingbird feeder weights, detects feeding events, and publishes data via MQTT to a central monitoring system.

## Build and Development Commands

**Build and Upload:**
- `pio run` - Build the project
- `pio run -t upload` - Upload via serial
- `pio run -e esp12e-ota` - Upload via OTA (Over-The-Air) to default device
- `pio run -e esp12e-ota-feeder1` - Upload to specific feeder (feeder1, feeder2, feeder3)

**Serial Monitoring:**
- `pio device monitor` - Monitor serial output at 115200 baud

**Environment Selection:**
- `nodemcuv2` - Development board with NodeMCU v2
- `esp12e` - Production ESP12E module via serial
- `esp12e-ota` - Production OTA updates to configurable IP
- `esp12e-ota-feederX` - Direct OTA to specific feeder by hostname

## Architecture

**Hardware Configuration:**
- HX711 load cell amplifier connected to GPIO 13 (dout) and 15 (sck)
- ESP8266 (NodeMCU v2 or ESP12E) with WiFi connectivity
- Load cell underneath feeder platform

**Feeder ID System:**
Each device is configured with a feeder ID (1, 2, 3) via build flags (`-D FM=X`):
- Controls hostname (`feedermonitorXX`)
- Sets unique calibration values for each load cell
- Defines MQTT topic hierarchy (`sensors/feeders/X/...`)

**MQTT Topics Structure:**
- `/status` - Device online/offline status with LWT
- `/reset` - Reset reason codes  
- `/raw_batch` - Batched raw weight readings (JSON arrays)
- `/average` - Calculated rolling average weight
- `/trigger` - Feeding event detection (true/false with delta)
- `/message` - Debug messages when enabled
- `/rssi_batch` - Batched WiFi signal strength data
- `/control` - Remote commands (reset)
- `/watchdog` - External watchdog ping to prevent resets

**Data Flow:**
1. Continuous load cell sampling with 5-sample averaging
2. Trigger detection based on weight delta thresholds (1.75g-40g range)
3. Batch publishing of raw data (256 samples) and RSSI data
4. Rolling average calculation with 0.9/0.1 weighting
5. OTA update capability for remote firmware updates

**Key Parameters:**
- Trigger sensitivity: 1.75g minimum, 40g maximum, 1.25g hysteresis
- Averaging window: 5 samples per reading
- Batch sizes: 256 samples for raw data, 256 for RSSI
- Watchdog timeout: 120 seconds (requires external ping)

**Calibration Process:**
Each feeder requires individual calibration using a 140.6g reference weight. Calibration values are hardcoded per feeder ID and stored in the firmware.