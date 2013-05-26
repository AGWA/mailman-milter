#include "utils.hpp"
#include <libmilter/mfapi.h>
#include <iostream>
#include <sstream>
#include <string>
#include <memory>
#include <string.h>
#include <errno.h>
#include <cstdio>
#include <cstdlib>
#include <stddef.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>
#include <set>
#include <vector>
#include <limits>

namespace {
	void print_usage (const char* argv0)
	{
		std::clog << "Usage: " << argv0 << " -s socket [-u user] [-g group] [-d] [-p pidfile] [-m socket-mode] [-v] [-S sender-action-script] mailing-list ..." << std::endl;
	}

	/*
	 * Global config
	 */
	bool				debug = false;
	std::set<std::string>		mailing_lists;
	std::string			sender_action_script = "/usr/lib/mailman/bin/sender_action";


	struct Error {
		std::string	message;

		explicit Error (const std::string& m) : message(m) { }

		sfsistat	handle () const
		{
			std::clog << "Error: " << message << std::endl;
			return SMFIS_TEMPFAIL;
		}
	};

	struct Message_context {
		std::vector<std::string>	env_rcpt;
		std::string			from;
	};

	struct Conn_context {
		std::auto_ptr<Message_context>	curr_message;

		void				start_message ()
		{
			curr_message.reset(new Message_context);
		}
		void				end_message ()
		{
			curr_message.reset();
		}
	};


	std::string canon_address (const char* addr)
	{
		// Strip pairs of leading and trailing angle brackets from the address
		const char*	start = addr;
		const char*	end = addr + std::strlen(addr);
		while (*start == '<' && *(end - 1) == '>') {
			++start;
			--end;
		}
		return std::string(start, end);
	}


	Conn_context*	get_conn (SMFICTX* ctx, bool message_context =false)
	{
		Conn_context*	conn_ctx = static_cast<Conn_context*>(smfi_getpriv(ctx));
		if (conn_ctx == NULL) {
			throw Error("No current connection context: smfi_getpriv failed");
		}
		if (message_context && !conn_ctx->curr_message.get()) {
			throw Error("No current message context");
		}
		return conn_ctx;
	}

	sfsistat on_connect (SMFICTX* ctx, char* hostname, struct sockaddr* hostaddr)
	{
		if (debug) std::cerr << "on_connect " << ctx << '\n';

		Conn_context*		conn_ctx = new Conn_context;
		if (smfi_setpriv(ctx, conn_ctx) == MI_FAILURE) {
			delete conn_ctx;
			std::clog << "on_connect: smfi_setpriv failed" << std::endl;
			return SMFIS_TEMPFAIL;
		}

		return SMFIS_CONTINUE;
	}

	sfsistat on_envfrom (SMFICTX* ctx, char** args)
	try {
		if (debug) std::cerr << "on_envfrom " << ctx << '\n';

		Conn_context*		conn_ctx = get_conn(ctx);

		// Create a new message context
		conn_ctx->start_message();

		return SMFIS_CONTINUE;
	} catch (Error e) {
		return e.handle();
	}

	sfsistat on_envrcpt (SMFICTX* ctx, char** args)
	try {
		if (debug) std::cerr << "on_envrcpt " << ctx << '\n';

		Conn_context*		conn_ctx = get_conn(ctx, true);
		conn_ctx->curr_message->env_rcpt.push_back(canon_address(args[0]));

		return SMFIS_CONTINUE;
	} catch (Error e) {
		return e.handle();
	}

	sfsistat on_header (SMFICTX* ctx, char* name, char* value)
	{
		if (debug) std::cerr << "on_header " << ctx << " \"" << name << "\" \"" << value << "\"\n";

		if (strcasecmp(name, "from") == 0) {
			Conn_context*		conn_ctx = NULL;

			try {
				conn_ctx = get_conn(ctx, true);

				conn_ctx->curr_message->from = value;
			} catch (Error e) {
				if (conn_ctx) { conn_ctx->end_message(); } // Torpedo the current message
				return e.handle();
			}
		}

		return SMFIS_CONTINUE;
	}

	sfsistat on_eom (SMFICTX* ctx)
	{
		if (debug) std::cerr << "on_eom " << ctx << '\n';

		sfsistat		milter_status = SMFIS_ACCEPT;
		Conn_context*		conn_ctx = NULL;
		try {
			conn_ctx = get_conn(ctx, true);

			if (conn_ctx->curr_message->env_rcpt.size() == 1 && mailing_lists.count(conn_ctx->curr_message->env_rcpt[0])) {
				/*
				 * This message is destined for a mailing list!
				 * Fork+exec mailman's sender_action script to determine what to do.
				 */
				pid_t		child = fork();
				if (child == -1) {
					std::perror("fork");
					throw Error("fork failed");
				} else if (child == 0) {
					close(0);
					close(1);
					close(2);
					open("/dev/null", O_RDONLY);
					open("/dev/null", O_WRONLY);
					open("/dev/null", O_WRONLY);
					execl(sender_action_script.c_str(), "sender_action", conn_ctx->curr_message->env_rcpt[0].c_str(), conn_ctx->curr_message->from.c_str(), NULL);
					_exit(-1);
				}

				// Wait for child to terminate
				int			wait_status;
				if (waitpid(child, &wait_status, 0) == -1) {
					std::perror("waitpid");
					throw Error("waitpid failed");
				}

				// Use exit code to determine fate of message:
				//   0 -> accept (mailman willing to accept message)
				//  65 -> accept (mailman wants to hold message for moderation)
				//  66 -> reject (mailman wants to reject message)
				//  67 -> reject (mailman wants to discard message)
				//  68 -> accept (we don't know what mailman wants to do, so we have to accept message)
				//  other exit code -> temp failure

				if (!WIFEXITED(wait_status)) {
					// Child did not exit cleanly
					std::clog << "Child did not exit cleanly.\n";
					milter_status = SMFIS_TEMPFAIL;
				} else if (WEXITSTATUS(wait_status) == 0 || WEXITSTATUS(wait_status) == 65 || WEXITSTATUS(wait_status) == 68) {
					// Accept the message
					if (debug) std::clog << "Accepting message.\n";
					milter_status = SMFIS_ACCEPT;
				} else if (WEXITSTATUS(wait_status) == 66 || WEXITSTATUS(wait_status) == 67) {
					// Reject the message
					if (debug) std::clog << "Rejecting message.\n";
					smfi_setreply(ctx, const_cast<char*>("550"), const_cast<char*>("5.7.1"), const_cast<char*>("Rejected by Mailman - please make sure you are sending from correct address"));
					milter_status = SMFIS_REJECT;
				} else {
					std::clog << "Child did not succeed.\n";
					milter_status = SMFIS_TEMPFAIL;
				}
			}

		} catch (Error e) {
			if (conn_ctx) { conn_ctx->end_message(); }
			return e.handle();
		}

		// ~~ We're done processing this message ~~
		conn_ctx->end_message();
		return milter_status;
	}

	sfsistat on_abort (SMFICTX* ctx)
	{
		if (debug) std::cerr << "on_abort " << ctx << '\n';
		if (Conn_context* conn_ctx = static_cast<Conn_context*>(smfi_getpriv(ctx))) {
			conn_ctx->end_message();
		}
		return SMFIS_CONTINUE; // return value doesn't matter
	}
	sfsistat on_close (SMFICTX* ctx)
	{
		if (debug) std::cerr << "on_close " << ctx << '\n';

		delete static_cast<Conn_context*>(smfi_getpriv(ctx));
		return SMFIS_CONTINUE; // return value doesn't matter
	}
}

int main (int argc, char** argv)
{
	std::string		socket_spec;
	std::string		user_name;
	std::string		group_name;
	bool			daemon = false;
	std::string		pid_file;
	int			socket_mode = -1;

	int		flag;
	while ((flag = getopt(argc, argv, "s:u:g:dp:m:vS:")) != -1) {
		switch (flag) {
		case 's':
			socket_spec = optarg;
			break;
		case 'u':
			user_name = optarg;
			break;
		case 'g':
			group_name = optarg;
			break;
		case 'd':
			daemon = true;
			break;
		case 'p':
			pid_file = optarg;
			break;
		case 'm':
			if (std::strlen(optarg) != 3 ||
					optarg[0] < '0' || optarg[0] > '7' ||
					optarg[1] < '0' || optarg[1] > '7' ||
					optarg[2] < '0' || optarg[2] > '7') {
				std::clog << "Invalid socket mode (not a 3 digit octal number): " << optarg << std::endl;
				return 2;
			}
			socket_mode = ((optarg[0] - '0') << 6) | ((optarg[1] - '0') << 3) | (optarg[2] - '0');
			break;
		case 'v':
			debug = true;
			break;
		case 'S':
			sender_action_script = optarg;
			break;
		default:
			print_usage(argv[0]);
			return 2;
		}
	}

	if (sender_action_script.empty() || socket_spec.empty() || argc - optind < 1) {
		print_usage(argv[0]);
		return 2;
	}
	if (access(sender_action_script.c_str(), X_OK) != 0) {
		std::perror(sender_action_script.c_str());
		std::clog << argv[0] << ": Specify the path to the sender_action script with -S option" << std::endl;
		return 1;
	}

	mailing_lists.insert(argv + optind, argv + argc);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_IGN);

	struct smfiDesc		milter_desc;
	std::memset(&milter_desc, '\0', sizeof(milter_desc));

	milter_desc.xxfi_name = const_cast<char*>("mailman-milter");
	milter_desc.xxfi_version = SMFI_VERSION;
	milter_desc.xxfi_flags = 0;
	milter_desc.xxfi_connect = on_connect;
	milter_desc.xxfi_helo = NULL;
	milter_desc.xxfi_envfrom = on_envfrom;
	milter_desc.xxfi_envrcpt = on_envrcpt;
	milter_desc.xxfi_header = on_header;
	milter_desc.xxfi_eoh = NULL;
	milter_desc.xxfi_body = NULL;
	milter_desc.xxfi_eom = on_eom;
	milter_desc.xxfi_abort = on_abort;
	milter_desc.xxfi_close = on_close;
	milter_desc.xxfi_unknown = NULL;
	milter_desc.xxfi_data = NULL;
	milter_desc.xxfi_negotiate = NULL;


	std::string		conn_spec;
	if (socket_spec[0] == '/') {
		// If the socket starts with a /, assume it's a path to a UNIX
		// domain socket and treat it specially.
		if (access(socket_spec.c_str(), F_OK) == 0) {
			std::clog << socket_spec << ": socket file already exists" << std::endl;
			return 1;
		}
		conn_spec = "unix:" + socket_spec;
	} else {
		conn_spec = socket_spec;
	}

	drop_privileges(user_name, group_name);

	if (daemon) {
		daemonize(pid_file, "");
	}

	if (socket_mode != -1) {
		// We don't have much control over the permissions of the socket, so
		// approximate it by setting a umask that should result in the desired
		// permissions on the socket.  This program doesn't create any other
		// files so this shouldn't have any undesired side-effects.
		umask(~socket_mode & 0777);
	}

	bool			ok = true;

	if (ok && smfi_setconn(const_cast<char*>(conn_spec.c_str())) == MI_FAILURE) {
		std::clog << "smfi_setconn failed" << std::endl;
		ok = false;
	}

	if (ok && smfi_register(milter_desc) == MI_FAILURE) {
		std::clog << "smfi_register failed" << std::endl;
		ok = false;
	}

	// Run the milter
	if (ok && smfi_main() == MI_FAILURE) {
		std::clog << "smfi_main failed" << std::endl;
		ok = false;
	}

	// Clean up

	if (socket_spec[0] == '/') {
		unlink(socket_spec.c_str());
	}
	if (!pid_file.empty()) {
		unlink(pid_file.c_str());
	}
       
	return ok ? 0 : 1;
}
