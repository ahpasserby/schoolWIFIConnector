#pragma once

#include <string>
#include <vector>

namespace sw::html {

struct Field {
  std::string name;
  std::string value;
  std::string type;  // lowercased; "" when the tag omitted it
};

struct Form {
  std::string action;
  std::string method = "get";  // lowercased
  std::string id;
  std::string name;
  std::vector<Field> fields;

  bool has_password() const;
  const Field *find(const std::string &field_name) const;
};

// A deliberately small, forgiving scanner. Campus portals ship malformed HTML
// far more often than not, so this never tries to build a real DOM: it locates
// <form> ... </form> spans and the <input>/<select> tags inside them.
std::vector<Form> extract_forms(const std::string &html);

// Redirect hints used to walk from a captive-portal splash page to the page
// that actually carries the login form.
std::string meta_refresh_url(const std::string &html);
std::string js_redirect_url(const std::string &html);
std::string iframe_src(const std::string &html);

std::string title(const std::string &html);
std::string tag_attr(const std::string &tag, const std::string &attr);

} // namespace sw::html
