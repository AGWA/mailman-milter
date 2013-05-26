#ifndef _UTILS_HPP
#define _UTILS_HPP

#include <string>

void drop_privileges (const std::string& username, const std::string& groupname);
void daemonize (const std::string& pid_file, const std::string& stderr_file);

#endif
