#include <Arduino.h>

#include <app.h>

#if defined(CONFIG_IDF_TARGET_ESP32C3)
  // my ESP32-C3 Super Mini
  const uint8_t PINS[LED_COUNT] = { 4, 5, 6, 7 };
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    #include <esp32-hal-rgb-led.h>
    const uint8_t pin = 48;  // WS2812 
#elif defined(ESP32)
  // my ESP32 Minikit 
  const uint8_t PINS[LED_COUNT] = { 22, 21, 17, 16 };
#elif defined(ESP8266)
  // Mini Board
  const uint8_t PINS[LED_COUNT] = { 4, 2, 12, 14 };
#endif

// Arduino 2 api: const uint8_t CHAN[LED_COUNT] = { 1, 2, 3, 4 };

#define PWM_FREQ 25000

#ifndef PWMRANGE
#define PWMRANGE 1023
#endif

#ifndef PWMBITS
#define PWMBITS 10
#endif

static bool isOn = true;
static uint32_t duty[LED_COUNT] = { 0 };

// ESP32 only thing?
#include <Preferences.h>
Preferences prefs;
static uint32_t duty_dirty[LED_COUNT] = { 0 };  // time of last duty change or 0 if no change since last save
uint32_t status_dirty = 0;  // time of last isOn change or 0 if no change since last save
static int duty_value[LED_COUNT] = { 0 };
static char slider[] = "sliderX";


static uint32_t value2duty( int value ) {
    #if defined(CONFIG_IDF_TARGET_ESP32S3)
        return map(value, 0, 1000, 0, 255);  // RGB
    #else
        // 0=0, 1=1, then duty ~ value^2 with duty=PWMRANGE for value=1000
        const int min_value = sqrt(PWMRANGE);
        if (value > 0) value += min_value;
        uint32_t new_duty = (PWMRANGE * value) / (1000 + min_value);
        new_duty *= new_duty;   // smaller duty has smaller steps...
        new_duty /= PWMRANGE;   // ...by quadratic function
        return new_duty;
    #endif
}

static void set_duty( led_t led, uint32_t new_duty ) {
    #if defined(CONFIG_IDF_TARGET_ESP32S3)
        if (isOn) {
            int r = map(duty[LED_R], 0, UINT8_MAX, 0, duty[LED_W]);
            int g = map(duty[LED_G], 0, UINT8_MAX, 0, duty[LED_W]);
            int b = map(duty[LED_B], 0, UINT8_MAX, 0, duty[LED_W]);
            rgbLedWrite(pin, r, g, b);
        }
        else {
            rgbLedWrite(pin, 0, 0, 0);
        }
    #elif defined(ESP32)
        ledcWrite(PINS[led], new_duty);
    #else
        analogWrite(PINS[led], new_duty);
    #endif
}


void app_value( led_t led, int value ) {
    if( value < 0 || value > 1000 ) return;  // for now slider should send promille (0..1000)
    uint32_t new_duty = value2duty(value);   // convert slider value to duty (0..PWMRANGE)
    if( new_duty != duty[led] ) {
        duty_dirty[led] = millis();  // start to accumulate rapid changes to save flash.
        if( !duty_dirty[led] ) duty_dirty[led]--;  // Make sure update time is never set to 0
        duty[led] = new_duty;
        duty_value[led] = value; // for making persistent later
    }
    if( isOn ) {
        set_duty(led, duty[led]);
    }
}

void setup_app( bool detach ) {
    prefs.begin(PROGNAME, false);
    isOn = prefs.getBool("on", true);
    int value[LED_COUNT];

    for( int i = LED_START; i < LED_COUNT; i++ ) {
        led_t led = static_cast<led_t>(i);
        value[led] = prefs.getInt(get_slider(i), 250);
    }

    #if defined(ESP32)
        for( int i = LED_START; i < LED_COUNT; i++ ) {
            led_t led = static_cast<led_t>(i);
            app_value(led, value[led]);
            #if !defined(CONFIG_IDF_TARGET_ESP32S3)
                if (detach) ledcDetach(PINS[led]);
                ledcAttach(PINS[led], PWM_FREQ, PWMBITS);
            #endif
        }
    #else
        analogWriteRange(PWMRANGE);
        for( int i = LED_START; i < LED_COUNT; i++ ) {
            led_t led = static_cast<led_t>(i);
            pinMode(PINS[led], OUTPUT);
            app_value(led, value[led]);
        }
    #endif
}

const char *get_slider( int led ) {
    slider[sizeof(slider)-2] = '0' + led;  // up to 10 sliders
    return slider;
}

bool handle_app() {
    for( int i = LED_START; i < LED_COUNT; i++ ) {
        led_t led = static_cast<led_t>(i);
        if( duty_dirty[led] && millis() - duty_dirty[led] > 1000 ) {
            // duty was last changed more than a second ago: save now
            prefs.putInt(get_slider(i), duty_value[led]);
            duty_dirty[led] = 0;
        }
    }
    if( status_dirty && millis() - status_dirty > 1000 ) {
        // isOn was last changed more than a second ago: save now
        prefs.putBool("on", isOn);
        status_dirty = 0;
    }
    return true;
}

bool app_status( bool status ) {
    if( status ) {  // button state changed to pressed -> toggle on/off
        isOn = !isOn;
        status_dirty = millis();  // start to accumulate rapid changes to save flash.
        if( !status_dirty ) status_dirty--;  // Make sure update time is never set to 0
        for( int i = LED_START; i < LED_COUNT; i++ ) {
            led_t led = static_cast<led_t>(i);
            if( isOn ) {
                set_duty(led, duty[led]);
            }
            else {
                set_duty(led, 0);
            }
        }
    }
    return isOn;
}

uint8_t get_pin( led_t led ) {
    #if defined(CONFIG_IDF_TARGET_ESP32S3)
        return pin;
    #else
        return PINS[led];
    #endif
}

int get_value( led_t led ) {
    return duty_value[led];
}

int get_duty( led_t led ) {
    return duty[led];
}

const char *get_duties() {
    static char duties[LED_COUNT * 6];
    char *str = duties;
    bool sep = false;
    for( int i = LED_START; i < LED_COUNT; i++ ) {
        led_t led = static_cast<led_t>(i);
        int n = snprintf(str, sizeof(duties) - (str - duties) - 1, "%s%d", sep ? "," : "", (int)duty[led]);
        str += n;
        sep = true;
    }
    return duties;
}

bool get_power() {
    return isOn;
}
