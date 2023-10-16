# Slider PWM

Use html range slider callbacks to control pwm duty cycle. 
The duty cycle (led brightnes) already changes while dragging the slider, not only after releasing the slider. 

![image](https://user-images.githubusercontent.com/32450554/218337707-5bb61d95-975d-4ba3-b0eb-d9acf8543b60.png)

## Circuit

![image](https://user-images.githubusercontent.com/32450554/218340075-af00e690-9560-4062-93e2-f44e5e82e25c.png)

When switching ~5A with the mosfet and no heatsink, it gets warm (~60°C). For more current a heatsink is advisable.

Warning: 
I used an ESP32 board with USB connector for initial programming. 
When connecting the USB port while ESP32-VDD was connected with Mini360-VO as described in the circuit diagram the ESP32 board LDO got very hot within seconds and the ESP32 did not boot. No power supply and no led strip was connected. Reason is unknown to me - I just avoided it.
  
## Build

### Prerequisite

* PlatformIO (e.g. as VS Code extension or standalone)
* ESP32, ESP-C3 (or maybe ESP8266)

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
