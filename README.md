# Water Level Sensor for Smart Irrigation Control

## Overview

Firmware for sensing the water level in a water container with a transducer based industrial liquid level sensor attached to a MCP3421 and read out by a ESP32 dev board. The sensor and ADC combination allows for sub-mm water level measurements.
It shows the water level on a little LCD screen and sends it over WiFi to the Golioth Cloud for low level and leakage alerts.

## Development
Follow the Zephyr setup guide and the Golioth Firmware SDK guide

https://docs.zephyrproject.org/latest/develop/getting_started/index.html

https://github.com/golioth/golioth-firmware-sdk