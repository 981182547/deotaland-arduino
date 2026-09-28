#pragma once
#include "agent_link.h"
#include "cloud_status.h"
// Internal WiFi worker API. Configuration is copied before the task is launched.
esp_err_t cloud_worker_start(const agent_platform_t* platform, const char* firmware);
void cloud_worker_network(bool ready);
void cloud_worker_stop();
bool cloud_worker_active();
void cloud_worker_status(agent_cloud_status_t* out);
esp_err_t cloud_worker_forget();
