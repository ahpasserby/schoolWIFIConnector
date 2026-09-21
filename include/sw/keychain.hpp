#pragma once

#include <string>

namespace sw::keychain {

// Generic-password items in the user's login keychain, keyed by
// (service, account). The LaunchAgent runs as the same user, so the daemon can
// read back what `schoolwifi setup` stored without a second prompt.
bool set_password(const std::string &service, const std::string &account,
                  const std::string &password, std::string *err);
bool get_password(const std::string &service, const std::string &account,
                  std::string *out, std::string *err);
bool delete_password(const std::string &service, const std::string &account, std::string *err);

} // namespace sw::keychain
