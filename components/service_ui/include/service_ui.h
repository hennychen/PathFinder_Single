#pragma once

#include <esp_err.h>

#include "app_pages.h"

esp_err_t service_ui_init(void);
void service_ui_show_page(app_page_id_t page);
void service_ui_render_system_page(void);
void service_ui_render_active_page(void);
