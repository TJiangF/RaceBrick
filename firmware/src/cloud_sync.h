#pragma once
#include <stdbool.h>

void cloud_sync_init(void);
bool cloud_sync_take_ready(void);
bool cloud_sync_ready(void);
void cloud_sync_summary(char *buf, int cap);
void cloud_sync_start(void);
void cloud_sync_cancel(void);
bool cloud_sync_take_result(char *buf, int cap);
const char *cloud_sync_uid(void);
