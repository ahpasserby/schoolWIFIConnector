#pragma once

#include <string>

namespace sw::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_level(Level level);
Level level();
void set_file(const std::string &path);
void set_color(bool enabled);

void debug(const std::string &msg);
void info(const std::string &msg);
void warn(const std::string &msg);
void error(const std::string &msg);

} // namespace sw::log
