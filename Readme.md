# Slider PWM

Use html range slider callbacks to control pwm duty cycle.

## ESP32/Arduino

### Prerequisite

* PlatformIO (e.g. as VS Code extension or standalone)
* ESP32 (or maybe ESP8266)

### Usage

* Edit platformio.ini: 
    * change upload and monitor ports to where the ESP is plugged in
    * change default env to what matches your ESP best
* Start build and upload
  ```
  pio run --target buildfs
  pio run --target uploadfs
  pio run --target upload
  ```
* Optional: check serial output to see what's going on on the ESP
  ```
  pio device monitor
  ```
* Connect to AP sliderpwm-1 and configure wifi via http://192.168.4.1
* Open http://sliderpwm-1 in a browser

## Other

* Uses Bootstrap (5.2.3) for flexible layout (served as local files. Size: ~60k)
* Uses JQuery (3.6.1) for post request on slider release (Size: ~30k)
* Uses base64 encoded favicon converted by https://www.base64-image.de/ (Size: ~300 bytes)
* Uses Preferences lib to store current duty cycle on changes
* Optional: the ESP will contact ntp, syslog, mqtt broker and influx db as a demo. See platformio.ini for configuration.