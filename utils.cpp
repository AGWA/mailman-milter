/*
 * Copyright 2013 Andrew Ayer
 *
 * This file is part of mailman-milter.
 *
 * mailman-milter is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mailman-milter is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mailman-milter.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "utils.hpp"
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>

void drop_privileges (const std::string& username, const std::string& groupname)
{
	if (username.empty() && groupname.empty()) {
		return;
	}

	struct passwd*		usr = NULL;
	struct group*		grp = NULL;
	if (!username.empty()) {
		errno = 0;
		if (!(usr = getpwnam(username.c_str()))) {
			std::clog << username << ": " << (errno ? strerror(errno) : "No such user") << std::endl;
			std::exit(1);
		}
	}

	if (!groupname.empty()) {
		errno = 0;
		if (!(grp = getgrnam(groupname.c_str()))) {
			std::clog << groupname << ": " << (errno ? strerror(errno) : "No such group") << std::endl;
			std::exit(1);
		}
	}

	// If no group is specified, but a user is specified, drop to the primary GID of that user
	if (setgid(grp ? grp->gr_gid : usr->pw_gid) == -1) {
		std::clog << "Failed to drop privileges: setgid: " << strerror(errno) << std::endl;
		std::exit(1);
	}

	if (usr) {
		if (initgroups(usr->pw_name, usr->pw_gid) == -1) {
			std::clog << "Failed to drop privileges: initgroups: " << strerror(errno) << std::endl;
			std::exit(1);
		}
		if (setuid(usr->pw_uid) == -1) {
			std::clog << "Failed to drop privileges: setuid: " << strerror(errno) << std::endl;
			std::exit(1);
		}
	}
}

void daemonize (const std::string& pid_file, const std::string& stderr_file)
{
	// Open the PID file (open before forking so we can report errors)
	std::ofstream	pid_out;
	if (!pid_file.empty()) {
		pid_out.open(pid_file.c_str(), std::ofstream::out | std::ofstream::trunc);
		if (!pid_out) {
			std::clog << "Unable to open PID file " << pid_file << " for writing." << std::endl;
			std::exit(1);
		}
	}

	// Open the file descriptor for stderr (open before forking so we can report errors)
	int		stderr_fd;
       	if (stderr_file.empty()) {
		stderr_fd = open("/dev/null", O_WRONLY);
	} else if ((stderr_fd = open(stderr_file.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0666)) == -1) {
		std::clog << "Failed to open " << stderr_file << ": " << strerror(errno) << std::endl;
		std::exit(1);
	}

	// Fork
	pid_t		pid = fork();
	if (pid == -1) {
		std::perror("fork");
		std::exit(127);
	}
	if (pid != 0) {
		// Exit parent
		_exit(0);
	}
	setsid();

	// Write the PID file now that we've forked
	if (pid_out) {
		pid_out << getpid() << '\n';
		pid_out.close();
	}

	// dup the stderr file to stderr
	if (stderr_fd != 2) {
		dup2(stderr_fd, 2);
		close(stderr_fd);
	}
	
	// dup stdin, stdout to /dev/null
	close(0);
	close(1);
	open("/dev/null", O_RDONLY);
	open("/dev/null", O_WRONLY);
}

