#pragma once

#include <LovyanGFX.hpp>

namespace ui::runway {

void drawLargeAirportRunways(lgfx::LGFXBase& gfx);

void getAirportHtml(const char* id, char* buff, size_t len);

}  // namespace ui::runway
