#include "app_pages.h"

static const char *k_page_names[PAGE_COUNT] = {
    "attitude",
    "compass",
    "navigation",
    "obd",
    "system",
    "voice",
    "inclinometer",
};

const char *app_pages_to_string(app_page_id_t page)
{
    if (page >= PAGE_COUNT) {
        return "unknown";
    }

    return k_page_names[page];
}

app_page_id_t app_pages_next(app_page_id_t page)
{
    if (page >= PAGE_INCLINE) {
        return PAGE_ATTITUDE;
    }

    return (app_page_id_t)(page + 1);
}

app_page_id_t app_pages_prev(app_page_id_t page)
{
    if (page == PAGE_ATTITUDE || page >= PAGE_COUNT) {
        return PAGE_INCLINE;
    }

    return (app_page_id_t)(page - 1);
}
