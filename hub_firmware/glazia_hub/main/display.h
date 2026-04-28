#pragma once

// Call once at boot
void display_init(void);

// Show two lines on the TFT — maps to sensor data panel
// line1 = data text, line2 = location/context (or NULL)
void display_show(const char *line1, const char *line2);

// Push hub location to the display — maps to hub_location_label
// Sends: HUB_LOC:<home_name>
void display_hub_location(const char *home_name);

// Push current sensor table to the display — maps to sensor_list_label
// Reads sensor state from espnow module and sends: SENSORS:S1|Unknown|ON;...
void display_sensor_list(void);

// Push the source sensor of the latest event — maps to hub_location_label
// Sends: SENSOR_LOC:<mac_str>
void display_sensor_location(const char *mac_str);
