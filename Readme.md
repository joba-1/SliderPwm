# Slider Callback

Example of a html range slider that calls back the server with the new slider value.

## Prerequisite

* Python
* Flask

e.g. use conda to install required modules:
```
conda create -n SliderCallback python flask
conda activate SliderCallback
```

## Usage

* Call `python SliderCallback.py` starts a flask webserver on port 5000
* Open the page http://localhost:5000
* Move the slider and watch the value change
* Note the server request with the new value on slider release

## Other

* Uses Bootstrap (5.2.3) for flexible layout (served as local files. Size: ~60k)
* Uses JQuery (3.6.1) for post request on slider release (Size: ~30k)
* Uses base64 encoded favicon converted by https://www.base64-image.de/ (Size: ~300 bytes)
* For callbacks while moving the slider, move code from onchange() to oninput()
* Should be easy to transfer to an ESP8266/ESP32 webserver
    * Partition layout with ~100k LittleFS/SPIFFS (or more)
    * AsyncWebserver for speed?