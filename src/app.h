#pragma once

typedef enum { LED_START, LED_R = LED_START, LED_G, LED_B, LED_W, LED_COUNT } led_t;

void setup_app();
bool handle_app();

bool app_status( bool onOff );
void app_value( led_t led, int value );

const char *get_slider( int led );

int get_duty( led_t led );
const char *get_duties();
bool get_power();