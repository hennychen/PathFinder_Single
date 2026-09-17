#pragma once

#include <stdint.h>

typedef enum {
    PAGE_ATTITUDE = 0,
    PAGE_COMPASS,
    PAGE_NAV,
    PAGE_OBD,
    PAGE_SYSTEM,
    PAGE_VOICE,
    PAGE_COUNT
} app_page_id_t;

const char *app_pages_to_string(app_page_id_t page);
app_page_id_t app_pages_next(app_page_id_t page);
app_page_id_t app_pages_prev(app_page_id_t page);
