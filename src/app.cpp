#include <Arduino.h>

#define PWM_PIN 5
#define PWM_CHANNEL 1

#define PWM_FREQ 25000

#ifndef PWMRANGE
#define PWMRANGE 1023
#endif

#ifndef PWMBITS
#define PWMBITS 10
#endif

static bool isOn = true;
static uint32_t duty = 0;

// ESP32 only thing?
#include <Preferences.h>
Preferences prefs;
uint32_t duty_dirty = 0;  // time of last duty change or 0 if no change since last save
uint32_t status_dirty = 0;  // time of last isOn change or 0 if no change since last save
int duty_value = 0;

static uint32_t value2duty( int value ) {
    uint32_t new_duty = (PWMRANGE * value) / 1000;
    new_duty *= new_duty;   // smaller duty has smaller steps...
    new_duty /= PWMRANGE;   // ...by quadratic function
    return new_duty;
}

static void set_duty( uint32_t new_duty ) {
    #if defined(ESP32)
        ledcWrite(PWM_CHANNEL, new_duty);
    #else
        analogWrite(PWM_PIN, new_duty);
    #endif
}


void app_value( int value ) {
    if( value < 0 || value > 1000 ) return;  // for now slider should send promille (0..1000)
    uint32_t new_duty = value2duty(value);   // convert slider value to duty (0..PWMRANGE)
    if( new_duty != duty ) {
        duty_dirty = millis();  // start to accumulate rapid changes to save flash.
        if( !duty_dirty ) duty_dirty--;  // Make sure update time is never set to 0
        duty = new_duty;
        duty_value = value; // for making persistent later
        if( isOn ) {
            set_duty(duty);
        }
    }
}

void setup_app() {
    #if defined(ESP32)
        ledcAttachPin(PWM_PIN, PWM_CHANNEL);
        ledcSetup(PWM_CHANNEL, PWM_FREQ, PWMBITS);
    #else
        analogWriteRange(PWMRANGE);
        pinMode(PWM_PIN, OUTPUT);
    #endif
    prefs.begin(PROGNAME, false);
    isOn = prefs.getBool("on", true);
    app_value(prefs.getInt("slider1", 250));
}

bool handle_app() {
    if( duty_dirty && millis() - duty_dirty > 1000 ) {
        // duty was last changed more than a second ago: save now
        prefs.putInt("slider1", duty_value);
        duty_dirty = 0;
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
        if( isOn ) {
            set_duty(duty);
        }
        else {
            set_duty(0);
        }
    }
    return isOn;
}

int get_duty() {
    return duty_value;
}

bool get_power() {
    return isOn;
}
