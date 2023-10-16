#pragma once

typedef enum { LED_START, LED_R = LED_START, LED_G, LED_B, LED_W, LED_COUNT } led_t;

void setup_app();
bool handle_app();

bool app_status( bool onOff );
void app_value( led_t led, int value );

const char *get_slider( int led );

uint8_t get_pin( led_t led );
int get_value( led_t led );  // slider value 0..1000
int get_duty( led_t led );   // pwm duty value 0..1023
const char *get_duties();
bool get_power();