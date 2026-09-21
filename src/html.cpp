#include "sw/html.hpp"

#include <cctype>

#include "sw/util.hpp"

namespace sw::html {
namespace {

// Finds the end of a tag started at `open`, tolerating quoted '>' inside
// attribute values.
std::size_t tag_end(const std::string &s, std::size_t open) {
  char quote = 0;
  for (std::size_t i = open; i < s.size(); ++i) {
    char c = s[i];
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return i;
    }
  }
  return std::string::npos;
}

// Locates `<name` as a real tag start (followed by whitespace, '>' or '/').
std::size_t find_tag(const std::string &s, const std::string &name, std::size_t from) {
  std::string needle = "<" + name;
  for (std::size_t i = util::ifind(s, needle, from); i != std::string::npos;
       i = util::ifind(s, needle, i + 1)) {
    std::size_t after = i + needle.size();
    if (after >= s.size()) return std::string::npos;
    char c = s[after];
    if (std::isspace(static_cast<unsigned char>(c)) || c == '>' || c == '/') return i;
  }
  return std::string::npos;
}

} // namespace

std::string tag_attr(const std::string &tag, const std::string &attr) {
  std::size_t pos = 0;
  while (pos < tag.size()) {
    std::size_t at = util::ifind(tag, attr, pos);
    if (at == std::string::npos) return "";
    pos = at + attr.size();

    // The match must be a whole attribute name, not a suffix of another one.
    if (at == 0 || !(std::isspace(static_cast<unsigned char>(tag[at - 1])) || tag[at - 1] == '<')) {
      continue;
    }
    std::size_t i = pos;
    while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
    if (i >= tag.size() || tag[i] != '=') continue;
    ++i;
    while (i < tag.size() && std::isspace(static_cast<unsigned char>(tag[i]))) ++i;
    if (i >= tag.size()) return "";

    if (tag[i] == '"' || tag[i] == '\'') {
      char quote = tag[i++];
      std::size_t close = tag.find(quote, i);
      if (close == std::string::npos) close = tag.size();
      return util::html_unescape(tag.substr(i, close - i));
    }
    std::size_t end = i;
    while (end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[end])) &&
           tag[end] != '>') {
      ++end;
    }
    return util::html_unescape(tag.substr(i, end - i));
  }
  return "";
}

bool Form::has_password() const {
  for (const Field &f : fields) {
    if (util::iequals(f.type, "password")) return true;
  }
  return false;
}

const Field *Form::find(const std::string &field_name) const {
  for (const Field &f : fields) {
    if (util::iequals(f.name, field_name)) return &f;
  }
  return nullptr;
}

namespace {

// Collects <input>/<select>/<textarea> controls inside one form body.
std::vector<Field> scan_fields(const std::string &body) {
  std::vector<Field> fields;

  for (std::size_t p = find_tag(body, "input", 0); p != std::string::npos;
       p = find_tag(body, "input", p + 1)) {
    std::size_t end = tag_end(body, p);
    if (end == std::string::npos) break;
    std::string tag = body.substr(p, end - p + 1);

    Field f;
    f.name = tag_attr(tag, "name");
    f.value = tag_attr(tag, "value");
    f.type = util::lower(tag_attr(tag, "type"));
    if (f.name.empty()) continue;

    // Unchecked boxes and radios are not submitted by a browser either.
    if ((f.type == "checkbox" || f.type == "radio") && !util::icontains(tag, "checked")) continue;
    if (f.type == "submit" || f.type == "button" || f.type == "image" || f.type == "reset") continue;

    fields.push_back(f);
    p = end;
  }

  for (std::size_t p = find_tag(body, "select", 0); p != std::string::npos;
       p = find_tag(body, "select", p + 1)) {
    std::size_t end = tag_end(body, p);
    if (end == std::string::npos) break;
    std::string tag = body.substr(p, end - p + 1);

    Field f;
    f.name = tag_attr(tag, "name");
    f.type = "select";
    if (f.name.empty()) continue;

    // Take the selected option, else the first one.
    std::size_t close = util::ifind(body, "</select", end);
    std::string inner = body.substr(end, close == std::string::npos ? std::string::npos : close - end);
    bool first_seen = false;
    for (std::size_t o = find_tag(inner, "option", 0); o != std::string::npos;
         o = find_tag(inner, "option", o + 1)) {
      std::size_t oend = tag_end(inner, o);
      if (oend == std::string::npos) break;
      std::string otag = inner.substr(o, oend - o + 1);
      std::string val = tag_attr(otag, "value");
      if (!first_seen) {
        f.value = val;
        first_seen = true;
      }
      if (util::icontains(otag, "selected")) {
        f.value = val;
        break;
      }
    }
    fields.push_back(f);
    p = end;
  }

  return fields;
}

} // namespace

std::vector<Form> extract_forms(const std::string &html) {
  std::vector<Form> forms;

  for (std::size_t p = find_tag(html, "form", 0); p != std::string::npos;
       p = find_tag(html, "form", p + 1)) {
    std::size_t open_end = tag_end(html, p);
    if (open_end == std::string::npos) break;

    std::string open_tag = html.substr(p, open_end - p + 1);
    std::size_t close = util::ifind(html, "</form", open_end);
    std::string body =
        html.substr(open_end + 1, close == std::string::npos ? std::string::npos : close - open_end - 1);

    Form form;
    form.action = tag_attr(open_tag, "action");
    form.method = util::lower(tag_attr(open_tag, "method"));
    if (form.method.empty()) form.method = "get";
    form.id = tag_attr(open_tag, "id");
    form.name = tag_attr(open_tag, "name");
    form.fields = scan_fields(body);

    forms.push_back(form);
    p = close == std::string::npos ? open_end : close;
  }

  // Some portals put the inputs outside any <form> and submit them with JS.
  // Expose those as a synthetic action-less form so the caller can still try.
  if (forms.empty()) {
    Form loose;
    loose.method = "post";
    loose.fields = scan_fields(html);
    if (!loose.fields.empty()) forms.push_back(loose);
  }

  return forms;
}

std::string meta_refresh_url(const std::string &html) {
  for (std::size_t p = find_tag(html, "meta", 0); p != std::string::npos;
       p = find_tag(html, "meta", p + 1)) {
    std::size_t end = tag_end(html, p);
    if (end == std::string::npos) break;
    std::string tag = html.substr(p, end - p + 1);
    if (!util::iequals(tag_attr(tag, "http-equiv"), "refresh")) continue;

    std::string content = tag_attr(tag, "content");
    std::size_t eq = util::ifind(content, "url=");
    if (eq == std::string::npos) continue;
    std::string url = util::trim(content.substr(eq + 4));
    if (!url.empty() && (url.front() == '"' || url.front() == '\'')) {
      char q = url.front();
      url = url.substr(1);
      std::size_t close = url.find(q);
      if (close != std::string::npos) url = url.substr(0, close);
    }
    return util::trim(url);
  }
  return "";
}

std::string js_redirect_url(const std::string &html) {
  // Matches the handful of shapes Chinese campus portals actually emit:
  //   location.href='...'  window.location="..."  top.self.location='...'
  //   location.replace('...')  window.location.assign("...")
  static const char *kPatterns[] = {
      "location.href", "location.replace", "location.assign", "self.location", "window.location",
      "top.location",  "location =",       "location=",
  };

  for (const char *pat : kPatterns) {
    std::size_t p = util::ifind(html, pat);
    while (p != std::string::npos) {
      std::size_t i = p + std::char_traits<char>::length(pat);
      // Skip past '=', '(' and whitespace to the opening quote.
      while (i < html.size() && (std::isspace(static_cast<unsigned char>(html[i])) ||
                                 html[i] == '=' || html[i] == '(')) {
        ++i;
      }
      if (i < html.size() && (html[i] == '"' || html[i] == '\'')) {
        char q = html[i++];
        std::size_t close = html.find(q, i);
        if (close != std::string::npos) {
          std::string url = util::trim(html.substr(i, close - i));
          // Ignore self-references and empty hops.
          if (!url.empty() && url != "#" && url != "/" && !util::starts_with(url, "javascript:")) {
            return util::html_unescape(url);
          }
        }
      }
      p = util::ifind(html, pat, p + 1);
    }
  }
  return "";
}

std::string iframe_src(const std::string &html) {
  // <frame> as well as <iframe>: portals old enough to use a frameset are
  // still in service, and the login page is the frame they point at.
  for (const char *tag_name : {"iframe", "frame"}) {
    for (std::size_t p = find_tag(html, tag_name, 0); p != std::string::npos;
         p = find_tag(html, tag_name, p + 1)) {
      std::size_t end = tag_end(html, p);
      if (end == std::string::npos) break;
      std::string src = tag_attr(html.substr(p, end - p + 1), "src");
      if (!src.empty() && src != "about:blank") return src;
    }
  }
  return "";
}

std::vector<std::string> script_srcs(const std::string &html) {
  std::vector<std::string> out;
  for (std::size_t p = find_tag(html, "script", 0); p != std::string::npos;
       p = find_tag(html, "script", p + 1)) {
    std::size_t end = tag_end(html, p);
    if (end == std::string::npos) break;
    std::string src = tag_attr(html.substr(p, end - p + 1), "src");
    if (!src.empty()) out.push_back(src);
    p = end;
  }
  return out;
}

bool submits_on_load(const std::string &html) {
  bool submits = util::icontains(html, ".submit()");
  bool on_load = util::icontains(html, "onload");
  return submits && on_load;
}

std::string title(const std::string &html) {
  std::size_t p = find_tag(html, "title", 0);
  if (p == std::string::npos) return "";
  std::size_t open_end = tag_end(html, p);
  if (open_end == std::string::npos) return "";
  std::size_t close = util::ifind(html, "</title", open_end);
  if (close == std::string::npos) return "";
  return util::trim(util::html_unescape(html.substr(open_end + 1, close - open_end - 1)));
}

} // namespace sw::html
