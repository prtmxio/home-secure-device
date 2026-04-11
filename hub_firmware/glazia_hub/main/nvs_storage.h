#pragma once
#include <stdbool.h>

void nvs_save_credentials(void);
bool nvs_load_credentials(void);
void nvs_clear_credentials(void);

// Persist paired sensor MAC list (call after each ESP-NOW ACK)
void nvs_save_sensors(const char macs[][18], int count);

// Load saved sensor MACs — returns count loaded (0 if none saved)
int  nvs_load_sensors(char macs[][18], int max_count);
