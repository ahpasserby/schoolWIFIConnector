#include "sw/util.hpp"

#include <termios.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

namespace sw::util {
namespace {

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

char lower_c(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// Length of a leading "scheme://", or 0 when the string does not start with
// one. Searching for "://" anywhere is wrong: captive portals routinely put a
// whole URL inside a relative one's query string
// (/byod/view/x.html?userurl=http://captive.apple.com/...), and treating that
// as absolute hands libcurl a URL with no host.
std::size_t scheme_prefix_length(const std::string &s) {
  std::size_t pos = s.find("://");
  if (pos == std::string::npos || pos == 0) return 0;
  if (!std::isalpha(static_cast<unsigned char>(s[0]))) return 0;
  for (std::size_t i = 1; i < pos; ++i) {
    char c = s[i];
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.')) {
      return 0;
    }
  }
  return pos + 3;
}

} // namespace

std::string trim(const std::string &s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), lower_c);
  return s;
}

bool iequals(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower_c(a[i]) != lower_c(b[i])) return false;
  }
  return true;
}

std::size_t ifind(const std::string &hay, const std::string &needle, std::size_t pos) {
  if (needle.empty()) return pos <= hay.size() ? pos : std::string::npos;
  if (needle.size() > hay.size()) return std::string::npos;
  for (std::size_t i = pos; i + needle.size() <= hay.size(); ++i) {
    std::size_t j = 0;
    while (j < needle.size() && lower_c(hay[i + j]) == lower_c(needle[j])) ++j;
    if (j == needle.size()) return i;
  }
  return std::string::npos;
}

bool icontains(const std::string &hay, const std::string &needle) {
  return ifind(hay, needle) != std::string::npos;
}

bool starts_with(const std::string &s, const std::string &prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream is(s);
  while (std::getline(is, cur, sep)) out.push_back(cur);
  return out;
}

std::string join(const std::vector<std::string> &parts, const std::string &sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) out += sep;
    out += parts[i];
  }
  return out;
}

std::string url_encode(const std::string &s) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

std::string url_decode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out += ' ';
    } else if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hex_val(s[i + 1]);
      int lo = hex_val(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
      } else {
        out += s[i];
      }
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string html_unescape(const std::string &s) {
  struct Entity {
    const char *name;
    const char *repl;
  };
  static const Entity kEntities[] = {
      {"&amp;", "&"},  {"&lt;", "<"},   {"&gt;", ">"},
      {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"},
      {"&nbsp;", " "},
  };
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '&') {
      bool matched = false;
      for (const Entity &e : kEntities) {
        std::size_t len = std::char_traits<char>::length(e.name);
        if (s.compare(i, len, e.name) == 0) {
          out += e.repl;
          i += len;
          matched = true;
          break;
        }
      }
      if (matched) continue;
    }
    out += s[i++];
  }
  return out;
}

std::string url_origin(const std::string &url) {
  std::size_t after_scheme = scheme_prefix_length(url);
  if (after_scheme == 0) return "";
  std::size_t slash = url.find('/', after_scheme);
  return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string url_without_query(const std::string &url) {
  std::size_t cut = url.find_first_of("?#");
  return cut == std::string::npos ? url : url.substr(0, cut);
}

UrlParts parse_url(const std::string &url) {
  UrlParts parts;
  std::string rest = url;

  std::size_t after_scheme = scheme_prefix_length(rest);
  if (after_scheme > 0) {
    parts.scheme = lower(rest.substr(0, after_scheme - 3));
    rest = rest.substr(after_scheme);
  } else {
    parts.scheme = "http";
  }

  std::size_t authority_end = rest.find_first_of("/?#");
  std::string authority =
      authority_end == std::string::npos ? rest : rest.substr(0, authority_end);

  std::size_t at = authority.rfind('@');  // drop any user:pass@
  if (at != std::string::npos) authority = authority.substr(at + 1);

  if (!authority.empty() && authority[0] == '[') {  // IPv6 literal
    std::size_t close = authority.find(']');
    if (close != std::string::npos) {
      parts.host = authority.substr(1, close - 1);
      std::size_t colon = authority.find(':', close);
      if (colon != std::string::npos) parts.port = authority.substr(colon + 1);
    }
  } else {
    std::size_t colon = authority.find(':');
    if (colon == std::string::npos) {
      parts.host = authority;
    } else {
      parts.host = authority.substr(0, colon);
      parts.port = authority.substr(colon + 1);
    }
  }

  if (parts.port.empty()) parts.port = parts.scheme == "https" ? "443" : "80";
  return parts;
}

std::string resolve_url(const std::string &base, const std::string &ref) {
  std::string r = trim(ref);
  if (r.empty()) return base;
  if (scheme_prefix_length(r) > 0) return r;
  if (starts_with(r, "//")) {
    std::size_t after_scheme = scheme_prefix_length(base);
    std::string proto = after_scheme == 0 ? "http:" : base.substr(0, after_scheme - 2);
    return proto + r;
  }
  std::string origin = url_origin(base);
  if (origin.empty()) return r;
  if (r[0] == '/') return origin + r;
  if (r[0] == '?' || r[0] == '#') {
    std::size_t cut = base.find_first_of("?#");
    return (cut == std::string::npos ? base : base.substr(0, cut)) + r;
  }
  // Relative to the base's directory.
  std::string path = base.substr(origin.size());
  std::size_t cut = path.find_first_of("?#");
  if (cut != std::string::npos) path = path.substr(0, cut);
  std::size_t slash = path.rfind('/');
  path = slash == std::string::npos ? "/" : path.substr(0, slash + 1);
  if (path.empty() || path[0] != '/') path = "/" + path;
  return origin + path + r;
}

std::string form_encode(const Pairs &pairs) {
  std::string out;
  for (const auto &kv : pairs) {
    if (!out.empty()) out += '&';
    out += url_encode(kv.first);
    out += '=';
    out += url_encode(kv.second);
  }
  return out;
}

std::string expand_vars(const std::string &tmpl, const std::map<std::string, std::string> &vars) {
  std::string out;
  for (std::size_t i = 0; i < tmpl.size();) {
    if (tmpl[i] != '{') {
      out += tmpl[i++];
      continue;
    }
    std::size_t close = tmpl.find('}', i);
    if (close == std::string::npos) {
      out += tmpl.substr(i);
      break;
    }
    std::string token = tmpl.substr(i + 1, close - i - 1);
    bool encode = false;
    std::size_t pipe = token.find('|');
    if (pipe != std::string::npos) {
      encode = iequals(trim(token.substr(pipe + 1)), "url");
      token = trim(token.substr(0, pipe));
    }
    auto it = vars.find(token);
    if (it == vars.end()) {
      out += tmpl.substr(i, close - i + 1);  // leave unknown placeholders alone
    } else {
      out += encode ? url_encode(it->second) : it->second;
    }
    i = close + 1;
  }
  return out;
}

std::string strip_jsonp(const std::string &body) {
  std::size_t open = body.find('(');
  std::size_t close = body.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close <= open) return body;
  return body.substr(open + 1, close - open - 1);
}

std::string json_field(const std::string &json, const std::string &key) {
  std::string needle = "\"" + key + "\"";
  std::size_t pos = json.find(needle);
  if (pos == std::string::npos) return "";

  std::size_t i = pos + needle.size();
  while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
  if (i >= json.size() || json[i] != ':') return "";
  ++i;
  while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
  if (i >= json.size()) return "";

  if (json[i] == '"') {
    ++i;
    std::string out;
    while (i < json.size() && json[i] != '"') {
      if (json[i] == '\\' && i + 1 < json.size()) ++i;  // keep escaped chars verbatim
      out += json[i++];
    }
    return out;
  }

  std::size_t end = i;
  while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
  return trim(json.substr(i, end - i));
}

namespace {

// Start and length of a JSON value's source text, or {npos, 0}.
std::pair<std::size_t, std::size_t> json_value_span(const std::string &json,
                                                    const std::string &key) {
  std::string needle = "\"" + key + "\"";
  std::size_t pos = json.find(needle);
  if (pos == std::string::npos) return {std::string::npos, 0};

  std::size_t i = pos + needle.size();
  while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
  if (i >= json.size() || json[i] != ':') return {std::string::npos, 0};
  ++i;
  while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
  if (i >= json.size()) return {std::string::npos, 0};

  std::size_t start = i;
  if (json[i] == '"') {
    ++i;
    while (i < json.size() && json[i] != '"') {
      if (json[i] == '\\' && i + 1 < json.size()) ++i;
      ++i;
    }
    if (i < json.size()) ++i;  // closing quote
    return {start, i - start};
  }
  while (i < json.size() && json[i] != ',' && json[i] != '}' && json[i] != ']') ++i;
  return {start, trim(json.substr(start, i - start)).size()};
}

} // namespace

std::string json_raw_field(const std::string &json, const std::string &key) {
  auto span = json_value_span(json, key);
  if (span.first == std::string::npos) return "";
  return trim(json.substr(span.first, span.second));
}

std::string base64_encode(const std::string &data) {
  static const char *alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t full = data.size() - data.size() % 3;

  for (std::size_t i = 0; i < full; i += 3) {
    std::uint32_t n = (static_cast<unsigned char>(data[i]) << 16) |
                      (static_cast<unsigned char>(data[i + 1]) << 8) |
                      static_cast<unsigned char>(data[i + 2]);
    out += alphabet[(n >> 18) & 63];
    out += alphabet[(n >> 12) & 63];
    out += alphabet[(n >> 6) & 63];
    out += alphabet[n & 63];
  }

  std::size_t rest = data.size() - full;
  if (rest == 1) {
    std::uint32_t n = static_cast<std::uint32_t>(static_cast<unsigned char>(data[full])) << 16;
    out += alphabet[(n >> 18) & 63];
    out += alphabet[(n >> 12) & 63];
    out += "==";
  } else if (rest == 2) {
    std::uint32_t n = (static_cast<std::uint32_t>(static_cast<unsigned char>(data[full])) << 16) |
                      (static_cast<std::uint32_t>(static_cast<unsigned char>(data[full + 1])) << 8);
    out += alphabet[(n >> 18) & 63];
    out += alphabet[(n >> 12) & 63];
    out += alphabet[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

std::string query_string(const std::string &url) {
  std::size_t q = url.find('?');
  if (q == std::string::npos) return "";
  std::string query = url.substr(q + 1);
  std::size_t hash = query.find('#');
  return hash == std::string::npos ? query : query.substr(0, hash);
}

std::string query_param(const std::string &url, const std::string &name) {
  for (const std::string &pair : split(query_string(url), '&')) {
    std::size_t eq = pair.find('=');
    if (eq == std::string::npos) continue;
    if (iequals(pair.substr(0, eq), name)) return url_decode(pair.substr(eq + 1));
  }
  return "";
}

std::string home_dir() {
  const char *h = std::getenv("HOME");
  return h ? std::string(h) : std::string();
}

std::string expand_tilde(const std::string &path) {
  if (path.empty() || path[0] != '~') return path;
  if (path.size() == 1) return home_dir();
  if (path[1] == '/') return home_dir() + path.substr(1);
  return path;
}

std::string dirname(const std::string &path) {
  std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

bool mkdir_p(const std::string &path) {
  if (path.empty() || path == "/" || path == ".") return true;
  struct stat st {};
  if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  if (!mkdir_p(dirname(path))) return false;
  return ::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
}

bool file_exists(const std::string &path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0;
}

bool write_file(const std::string &path, const std::string &data) {
  if (!mkdir_p(dirname(path))) return false;
  std::ofstream os(path, std::ios::binary | std::ios::trunc);
  if (!os) return false;
  os.write(data.data(), static_cast<std::streamsize>(data.size()));
  return os.good();
}

std::string read_file(const std::string &path) {
  std::ifstream is(path, std::ios::binary);
  if (!is) return "";
  std::ostringstream ss;
  ss << is.rdbuf();
  return ss.str();
}

namespace {
std::string format_time(const char *fmt) {
  std::time_t t = std::time(nullptr);
  std::tm tm {};
  localtime_r(&t, &tm);
  char buf[64];
  std::strftime(buf, sizeof(buf), fmt, &tm);
  return buf;
}
} // namespace

std::string now_iso() { return format_time("%Y-%m-%d %H:%M:%S"); }
std::string now_compact() { return format_time("%Y%m%d-%H%M%S"); }

std::string exec_capture(const std::string &cmd) {
  FILE *pipe = ::popen(cmd.c_str(), "r");
  if (!pipe) return "";
  std::string out;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
  ::pclose(pipe);
  return out;
}

std::string read_line(const std::string &prompt, const std::string &fallback) {
  std::fputs(prompt.c_str(), stdout);
  std::fflush(stdout);
  std::string line;
  if (!std::getline(std::cin, line)) return fallback;
  line = trim(line);
  return line.empty() ? fallback : line;
}

std::string read_password(const std::string &prompt) {
  std::fputs(prompt.c_str(), stdout);
  std::fflush(stdout);

  termios old {};
  bool tty = ::tcgetattr(STDIN_FILENO, &old) == 0;
  if (tty) {
    termios quiet = old;
    quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
  }

  std::string line;
  std::getline(std::cin, line);

  if (tty) {
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    std::fputs("\n", stdout);
  }
  return line;
}

} // namespace sw::util
