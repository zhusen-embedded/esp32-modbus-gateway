#ifndef MQTT_UP_H
#define MQTT_UP_H

#include <stdint.h>
#include <stddef.h>

void mqtt_up_init(const char *uri, const char *ca_pem);
int  mqtt_up_publish_json(const char *json);
int  mqtt_up_connected(void);
int  mqtt_up_ready(void);       /* 连上并且订阅完成 */
uint32_t mqtt_up_pub_ok(void);  /* 累计成功上报条数，用于现场核对 */
uint32_t mqtt_up_pub_fail(void);

#endif
