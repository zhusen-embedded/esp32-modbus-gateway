#ifndef MQTT_UP_H
#define MQTT_UP_H

#include <stdint.h>

void mqtt_up_init(const char *uri, const char *ca_pem);
int  mqtt_up_publish_json(const char *json);
int  mqtt_up_connected(void);

#endif
