#pragma once
#include <string_view>

namespace kharvoxnative::log {
void init();
void shutdown();
void info(std::string_view msg);
void warn(std::string_view msg);
void error(std::string_view msg);
}
