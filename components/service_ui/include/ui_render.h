#pragma once

#include "app_pages.h"
#include "app_state.h"
#include "ui_pages.h"

void ui_render_page(app_page_id_t page, const workflow_state_t *state, ui_widgets_t *w);
