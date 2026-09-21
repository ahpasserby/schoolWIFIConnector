#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sw::util {

std::string trim(const std::string &s);
std::string lower(std::string s);
bool iequals(const std::string &a, const std::string &b);
std::size_t ifind(const std::string &hay, const std::string &needle, std::size_t pos = 0);
bool icontains(const std::string &hay, const std::string &needle);
bool starts_with(const std::string &s, const std::string &prefix);

std::vector<std::string> split(const std::string &s, char sep);
std::string join(const std::vector<std::string> &parts, const std::string &sep);

std::string url_encode(const std::string &s);
std::string url_decode(const std::string &s);
std::string html_unescape(const std::string &s);

// Resolves `ref` (absolute, protocol-relative, root-relative or path-relative)
// against `base`. Returns `ref` unchanged when it cannot be interpreted.
std::string resolve_url(const std::string &base, const std::string &ref);
std::string url_origin(const std::string &url);

using Pairs = std::vector<std::pair<std::string, std::string>>;
std::string form_encode(const Pairs &pairs);

// Replaces {name} placeholders. Adds {name|url} for a percent-encoded variant.
std::string expand_vars(const std::string &tmpl, const std::map<std::string, std::string> &vars);

std::string home_dir();
std::string expand_tilde(const std::string &path);
std::string dirname(const std::string &path);
bool mkdir_p(const std::string &path);
bool file_exists(const std::string &path);
bool write_file(const std::string &path, const std::string &data);
std::string read_file(const std::string &path);

std::string now_iso();
std::string now_compact();

// Runs a command via /bin/sh and captures stdout. Returns "" on failure.
std::string exec_capture(const std::string &cmd);

// Reads a line from the terminal with echo disabled.
std::string read_password(const std::string &prompt);
std::string read_line(const std::string &prompt, const std::string &fallback = "");

} // namespace sw::util
