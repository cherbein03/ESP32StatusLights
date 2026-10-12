ESP32-H2-Zero Hubitat Status Lights
===================================

This ESP-IDF project turns the Waveshare ESP32-H2-Zero into a mains-powered
Zigbee router with four Home Automation On/Off Light endpoints. Hubitat can
control each endpoint independently.

GPIO mapping
------------

Endpoint 10 / LED 1: GPIO10
Endpoint 11 / LED 2: GPIO11
Endpoint 12 / LED 3: GPIO12
Endpoint 13 / LED 4: GPIO13
Endpoint 14 / LED 5: GPIO14
Endpoint 15 / LED 6: GPIO22
Pairing reset button: GPIO0 to GND
Onboard WS2812 RGB status LED: GPIO8

At boot, all six LEDs turn on for two seconds, then turn off. The Zigbee On/Off
attributes start in the off state so Hubitat can control them independently.

Connect each GPIO to an LED anode through its own current-limiting resistor
(330 ohms is a suitable starting value), and connect each LED cathode to GND.
These are logic outputs only: do not connect bare LEDs without resistors, or
drive LED strips, relays, or other high-current loads directly from a GPIO.
The default outputs are active-high. Confirm the board revision's pinout before
wiring; change the GPIO mapping in main/main.c if needed.

Connect a normally-open momentary button between GPIO0 and GND. The firmware
uses the internal pull-up; hold the button for 3 seconds to clear saved Zigbee
network data and restart pairing. Put Hubitat into Zigbee inclusion mode before
pressing the button. This is an external button, not the board's BOOT button.

The onboard RGB LED blinks blue during startup and Zigbee pairing, turns solid
green after joining, and turns off after 10 seconds.

Build and flash
---------------

Open this folder in VS Code with the Espressif ESP-IDF extension configured.
The project target is ESP32-H2. Use the ESP-IDF extension to build, select the
board's serial port, flash, and open the monitor.

Pairing with Hubitat
--------------------

1. Put Hubitat into Zigbee inclusion mode before powering or resetting the
	board. On first startup, it scans for an existing Zigbee network and joins
	as a router. Keep USB power connected for normal operation.
2. On the Hubitat device page, set Type to **Generic Zigbee Multi-Endpoint
	Switch**, then select **Save Device**.
3. Select **Configure**. Hubitat should create six child switch devices, one
	for each On/Off Light endpoint (10-15). Use those child switches to control
	the LEDs independently. Do not use a Zigbee button-controller driver; it
	reports physical button presses and does not control these LED endpoints.

4. For a fresh pairing after the device has joined another network, enable
	Hubitat inclusion and hold the GPIO0 button for 3 seconds.

Use separate resistor-protected indicator LEDs for initial testing. This
firmware does not implement dimming, color control, or high-current switching.
