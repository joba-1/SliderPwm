#pragma once

void setup_app();
bool handle_app();

bool app_status( bool onOff );
void app_value( int value );

int get_duty();
bool get_power();