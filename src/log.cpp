#include "sw/log.hpp"

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <mutex>

#include "sw/util.hpp"

namespace sw::log {
namespace {

Level g_level = Level::Info;
std::string g_file;
bool g_color = ::isatty(STDERR_FILENO) != 0;
std::mutex g_mutex;

const char *name_of(Level l) {
  switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
  }
  return "?????";
}

const char *color_of(Level l) {
  switch (l) {
    case Level::Debug: return "\033[90m";
    case Level::Info: return "\033[36m";
    case Level::Warn: return "\033[33m";
    case Level::Error: return "\033[31m";
  }
  return "";
}

void emit(Level l, const std::string &msg) {
  if (l < g_level) return;
  std::lock_guard<std::mutex> lock(g_mutex);

  if (g_color) {
    std::fprintf(stderr, "%s%s\033[0m %s\n", color_of(l), name_of(l), msg.c_str());
  } else {
    std::fprintf(stderr, "%s %s\n", name_of(l), msg.c_str());
  }

  if (!g_file.empty()) {
    std::ofstream os(g_file, std::ios::app);
    if (os) os << util::now_iso() << " " << name_of(l) << " " << msg << "\n";
  }
}

} // namespace

void set_level(Level level) { g_level = level; }
Level level() { return g_level; }
void set_color(bool enabled) { g_color = enabled; }

void set_file(const std::string &path) {
  g_file = util::expand_tilde(path);
  if (!g_file.empty()) util::mkdir_p(util::dirname(g_file));
}

void debug(const std::string &msg) { emit(Level::Debug, msg); }
void info(const std::string &msg) { emit(Level::Info, msg); }
void warn(const std::string &msg) { emit(Level::Warn, msg); }
void error(const std::string &msg) { emit(Level::Error, msg); }

} // namespace sw::log
