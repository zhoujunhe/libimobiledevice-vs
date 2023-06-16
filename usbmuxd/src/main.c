/*
 * main.c
 *
 * Copyright (C) 2009-2019 Nikias Bassen <nikias@gmx.li>
 * Copyright (C) 2013-2014 Martin Szulecki <m.szulecki@libimobiledevice.org>
 * Copyright (C) 2009 Hector Martin <hector@marcansoft.com>
 * Copyright (C) 2009 Paul Sladen <libiphone@paul.sladen.org>
 * Copyright (C) 2014 Frederik Carlier <frederik.carlier@quamotion.mobi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 or version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef _MSC_VER
#include "config_msc.h"
#include <direct.h>
#endif

#ifdef WIN32
#include <winsock2.h>
#include "winsock2-ext.h"
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/resource.h>
#include <pwd.h>
#include <grp.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <getopt.h>

#include "log.h"
#include "usb.h"
#include "device.h"
#include "client.h"
#include "conf.h"
#include <pthread.h>
#include <signal.h>
#include "usbmuxd-proto.h"

#ifdef _MSC_VER
#include <libusb-1.0/libusb.h>
#else
#include <libusb.h>
#endif

#ifdef HAVE_LIBIMOBILEDEVICE
#include "libimobiledevice/libimobiledevice.h"
#endif

static const char *socket_path = "/var/run/usbmuxd";
static const char *lockfile = "/var/run/usbmuxd.pid";

// Global state used in other files
int should_exit;
int should_discover;
int use_logfile = 0;
int no_preflight = 0;

#if DEBUG || _DEBUG
static int verbose = 255;
static int libusb_verbose = 255;
#else
static int verbose = 0;
static int libusb_verbose = 0;
#endif

// Global state for main.c
static int foreground = 0;
static int tcp = 0;
static int all_interfaces = 0;
static int drop_privileges = 0;
static const char *drop_user = NULL;
static int opt_disable_hotplug = 0;
static int opt_enable_exit = 0;
static int opt_exit = 0;
static int exit_signal = 0;
static int daemon_pipe;

static int report_to_parent = 0;

#ifdef WIN32
#define socket_handle SOCKET
#else
#define socket_handle int
#endif

static socket_handle create_socket(void) {
	struct sockaddr_un bind_addr;
	struct sockaddr_in tcp_addr;
	socket_handle listenfd;

#ifdef WIN32
	int ret;
	WSADATA wsaData = { 0 };

	// Initialize Winsock
	usbmuxd_log(LL_INFO, "Starting WSA");
	ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (ret != 0) {
		usbmuxd_log(LL_FATAL, "ERROR: WSAStartup failed: %s", strerror(ret));
		return 1;
	}

	usbmuxd_log(LL_INFO, "Opening socket");
	listenfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenfd == INVALID_SOCKET) {
		usbmuxd_log(LL_FATAL, "ERROR: socket() failed: %s", WSAGetLastError());
	}
#else
	if (!tcp) {
		if (unlink(socket_path) == -1 && errno != ENOENT) {
			usbmuxd_log(LL_FATAL, "unlink(%s) failed: %s", socket_path, strerror(errno));
			return -1;
		}

		listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (listenfd == -1) {
			usbmuxd_log(LL_FATAL, "ERROR: socket() failed: %s", strerror(errno));
			return -1;
		}
	} else {
		listenfd = socket(AF_INET, SOCK_STREAM, 0);
		if (listenfd == -1) {
			usbmuxd_log(LL_FATAL, "ERROR: socket() failed: %s", strerror(errno));
			return -1;
		}
	}
#endif

#ifdef WIN32
	u_long iMode = 1;

	usbmuxd_log(LL_INFO, "Setting socket to non-blocking");
	ret = ioctlsocket(listenfd, FIONBIO, &iMode);
	if (ret != NO_ERROR) {
		usbmuxd_log(LL_FATAL, "ERROR: Could not set socket to non blocking: %d", ret);
		return -1;
	}
#else
	int flags = fcntl(listenfd, F_GETFL, 0);
	if (flags < 0) {
		usbmuxd_log(LL_FATAL, "ERROR: Could not get flags for socket");
	} else {
		if (fcntl(listenfd, F_SETFL, flags | O_NONBLOCK) < 0) {
			usbmuxd_log(LL_FATAL, "ERROR: Could not set socket to non-blocking");
		}
	}
#endif

	memset(&bind_addr, 0, sizeof(bind_addr));

#ifdef WIN32
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = all_interfaces == 0 ? inet_addr("127.0.0.1") : inet_addr("0.0.0.0");
	bind_addr.sin_port = htons(USBMUXD_SOCKET_PORT);
#else
	if (tcp) {
		usbmuxd_log(LL_INFO, "Preparing a TCP socket");
		tcp_addr.sin_family = AF_INET;
		tcp_addr.sin_addr.s_addr = all_interfaces == 0 ? inet_addr("127.0.0.1") : inet_addr("0.0.0.0");
		tcp_addr.sin_port = htons(USBMUXD_SOCKET_PORT);
	} else {
		usbmuxd_log(LL_INFO, "Preparing a Unix socket");
		bind_addr.sun_family = AF_UNIX;
		strcpy(bind_addr.sun_path, socket_path);
	}
#endif

	usbmuxd_log(LL_INFO, "Binding to socket");
#ifdef WIN32
	if (bind(listenfd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
		usbmuxd_log(LL_FATAL, "bind() failed. WSAGetLastError returned error code %u. Is another process using TCP port %d?", WSAGetLastError(), USBMUXD_SOCKET_PORT);
		return -1;
	}
#else
	if (tcp) {
		if (bind(listenfd, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr)) != 0) {
			usbmuxd_log(LL_FATAL, "bind() on a Unix socket failed: %s", strerror(errno));
			return -1;
		}
	} else {
		if (bind(listenfd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
			usbmuxd_log(LL_FATAL, "bind() on a TCP socket failed: %s", strerror(errno));
			return -1;
		}
	}
#endif

	// Start listening
	usbmuxd_log(LL_INFO, "Starting to listen on socket");
	if (listen(listenfd, 5) != 0) {
		usbmuxd_log(LL_FATAL, "listen() failed: %s", strerror(errno));
		return -1;
	}

#ifndef WIN32
	chmod(socket_path, 0666);
#endif

	return listenfd;
}

#ifdef WIN32
#else
static void handle_signal(int sig)
{
	if (sig != SIGUSR1 && sig != SIGUSR2) {
		usbmuxd_log(LL_NOTICE,"Caught signal %d, exiting", sig);
		should_exit = 1;
	} else {
		if(opt_enable_exit) {
			if (sig == SIGUSR1) {
				usbmuxd_log(LL_INFO, "Caught SIGUSR1, checking if we can terminate (no more devices attached)...");
				if (device_get_count(1) > 0) {
					// we can't quit, there are still devices attached.
					usbmuxd_log(LL_NOTICE, "Refusing to terminate, there are still devices attached. Kill me with signal 15 (TERM) to force quit.");
				} else {
					// it's safe to quit
					should_exit = 1;
				}
			} else if (sig == SIGUSR2) {
				usbmuxd_log(LL_INFO, "Caught SIGUSR2, scheduling device discovery");
				should_discover = 1;
			}
		} else {
			usbmuxd_log(LL_INFO, "Caught SIGUSR1/2 but this instance was not started with \"--enable-exit\", ignoring.");
		}
	}
}

static void set_signal_handlers(void)
{
	struct sigaction sa;
	sigset_t set;

	// Mask all signals we handle. They will be unmasked by ppoll().
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGQUIT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	sigprocmask(SIG_SETMASK, &set, NULL);

	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = handle_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
}
#endif

#if(!HAVE_PPOLL && !WIN32)
static int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *timeout, const sigset_t *sigmask)
{
	int ready;
	sigset_t origmask;
	int to = timeout->tv_sec*1000 + timeout->tv_nsec/1000000;

	sigprocmask(SIG_SETMASK, sigmask, &origmask);
	ready = poll(fds, nfds, to);
	sigprocmask(SIG_SETMASK, &origmask, NULL);

	return ready;
}
#endif

#ifdef WIN32
static int ppoll(struct WSAPoll *fds, nfds_t nfds, int timeout)
{
	return WSAPoll(fds, nfds, timeout);
}
#endif

static int main_loop(int listenfd)
{
	int to, cnt, i, dto;
	struct fdlist pollfds;

#ifndef WIN32
	struct timespec tspec;

	sigset_t empty_sigset;
	sigemptyset(&empty_sigset); // unmask all signals
#endif

	fdlist_create(&pollfds);
	while(!should_exit) {
		usbmuxd_log(LL_FLOOD, "main_loop iteration");
#ifndef WIN32
		to = usb_get_timeout();
		usbmuxd_log(LL_FLOOD, "USB timeout is %d ms", to);
		dto = device_get_timeout();
		usbmuxd_log(LL_FLOOD, "Device timeout is %d ms", dto);
		if(dto < to)
			to = dto;
#endif

		fdlist_reset(&pollfds);
		fdlist_add(&pollfds, FD_LISTEN, listenfd, POLLIN);

#ifndef WIN32
		// Polling of USB events is not available through libusb on Windows,
		// see http://libusb.org/static/api-1.0/group__poll.html
		usb_get_fds(&pollfds);
#endif

		client_get_fds(&pollfds);
		usbmuxd_log(LL_FLOOD, "fd count is %d", pollfds.count);

#ifdef WIN32
		cnt = ppoll(pollfds.fds, pollfds.count, 100);
		usb_process();
#else
		tspec.tv_sec = to / 1000;
		tspec.tv_nsec = (to % 1000) * 1000000;
		cnt = ppoll(pollfds.fds, pollfds.count, &tspec, &empty_sigset);
#endif

		usbmuxd_log(LL_FLOOD, "poll() returned %d", cnt);
		if(cnt == -1) {
			if(errno == EINTR) {
				if(should_exit) {
					usbmuxd_log(LL_INFO, "Event processing interrupted");
					break;
				}
				if(should_discover) {
					should_discover = 0;
					usbmuxd_log(LL_INFO, "Device discovery triggered");
					usb_discover();
				}
			}
#ifndef WIN32
		} else if(cnt == 0) {
			if(usb_process() < 0) {
				usbmuxd_log(LL_FATAL, "usb_process() failed");
				fdlist_free(&pollfds);
				return -1;
			}
			device_check_timeouts();
#endif
		} else {
			int done_usb = 0;
			for(i=0; i<pollfds.count; i++) {
				if(pollfds.fds[i].revents) {
					if(!done_usb && pollfds.owners[i] == FD_USB) {
						if(usb_process() < 0) {
							usbmuxd_log(LL_FATAL, "usb_process() failed");
							fdlist_free(&pollfds);
							return -1;
						}
						done_usb = 1;
					}
					if(pollfds.owners[i] == FD_LISTEN) {
						if(client_accept(listenfd) < 0) {
							usbmuxd_log(LL_FATAL, "client_accept() failed");
							fdlist_free(&pollfds);
							return -1;
						}
					}
					if(pollfds.owners[i] == FD_CLIENT) {
						client_process(pollfds.fds[i].fd, pollfds.fds[i].revents);
					}
				}
			}
		}
	}
	fdlist_free(&pollfds);
	return 0;
}

/**
 * make this program run detached from the current console
 */
#ifdef WIN32
#else
static int daemonize(void)
{
	pid_t pid;
	pid_t sid;
	int pfd[2];
	int res;

	// already a daemon
	if (getppid() == 1)
		return 0;

	if((res = pipe(pfd)) < 0) {
		usbmuxd_log(LL_FATAL, "pipe() failed.");
		return res;
	}

	pid = fork();
	if (pid < 0) {
		usbmuxd_log(LL_FATAL, "fork() failed.");
		return pid;
	}

	if (pid > 0) {
		// exit parent process
		int status;
		close(pfd[1]);

		if((res = read(pfd[0],&status,sizeof(int))) != sizeof(int)) {
			fprintf(stderr, "usbmuxd: ERROR: Failed to get init status from child, check syslog for messages.\n");
			exit(1);
		}
		if(status != 0)
			fprintf(stderr, "usbmuxd: ERROR: Child process exited with error %d, check syslog for messages.\n", status);
		exit(status);
	}
	// At this point we are executing as the child process
	// but we need to do one more fork

	daemon_pipe = pfd[1];
	close(pfd[0]);
	report_to_parent = 1;

	// Create a new SID for the child process
	sid = setsid();
	if (sid < 0) {
		usbmuxd_log(LL_FATAL, "setsid() failed.");
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		usbmuxd_log(LL_FATAL, "fork() failed (second).");
		return pid;
	}

	if (pid > 0) {
		// exit parent process
		close(daemon_pipe);
		exit(0);
	}

	// Change the current working directory.
	if ((chdir("/")) < 0) {
		usbmuxd_log(LL_FATAL, "chdir() failed");
		return -2;
	}
	// Redirect standard files to /dev/null
	if (!freopen("/dev/null", "r", stdin)) {
		usbmuxd_log(LL_FATAL, "Redirection of stdin failed.");
		return -3;
	}
	if (!freopen("/dev/null", "w", stdout)) {
		usbmuxd_log(LL_FATAL, "Redirection of stdout failed.");
		return -3;
	}

	return 0;
}

static int notify_parent(int status)
{
	int res;

	report_to_parent = 0;
	if ((res = write(daemon_pipe, &status, sizeof(int))) != sizeof(int)) {
		usbmuxd_log(LL_FATAL, "Could not notify parent!");
		if(res >= 0)
			return -2;
		else
			return res;
	}
	close(daemon_pipe);
	if (!freopen("/dev/null", "w", stderr)) {
		usbmuxd_log(LL_FATAL, "Redirection of stderr failed.");
		return -1;
	}
	return 0;
}
#endif

static void usage()
{
	printf("Usage: %s [OPTIONS]\n", PACKAGE_NAME);
	printf("\n");
	printf("Expose a socket to multiplex connections from and to iOS devices.\n");
	printf("\n");
	printf("OPTIONS:\n");
	printf("  -h, --help\t\tPrint this message.\n");
	printf("  -v, --verbose\t\tBe verbose (use twice or more to increase).\n");
	printf("  -w, --verbose-usb\tEnable libusb logging.\n");
	printf("  -t, --tcp\t\tCreate a TCP socket (default on Windows).\n");
	printf("  -a, --all-interfaces\tCListen on all interfaces for connections (TCP only).\n");
	printf("  -f, --foreground\tDo not daemonize (implies one -v).\n");
	printf("  -U, --user USER\tChange to this user after startup (needs USB privileges).\n");
	printf("  -n, --disable-hotplug\tDisables automatic discovery of devices on hotplug.\n");
	printf("                       \tStarting another instance will trigger discovery instead.\n");
	printf("  -z, --enable-exit\tEnable \"--exit\" request from other instances and exit\n");
	printf("                   \tautomatically if no device is attached.\n");
	printf("  -p, --no-preflight\tDisable lockdownd preflight on new device.\n");
#ifdef HAVE_UDEV
	printf("  -u, --udev\t\tRun in udev operation mode (implies -n and -z).\n");
#endif
#ifdef HAVE_SYSTEMD
	printf("  -s, --systemd\t\tRun in systemd operation mode (implies -z and -f).\n");
#endif

#ifdef WIN32
#else
	printf("  -x, --exit\t\tNotify a running instance to exit if there are no devices\n");
	printf("            \t\tconnected (sends SIGUSR1 to running instance) and exit.\n");
	printf("  -X, --force-exit\tNotify a running instance to exit even if there are still\n");
	printf("                  \tdevices connected (always works) and exit.\n");
#endif

	printf("  -l, --logfile=LOGFILE\tLog (append) to LOGFILE instead of stderr or syslog.\n");
	printf("  -V, --version\t\tPrint version information and exit.\n");
	printf("\n");
	printf("Homepage:    <" PACKAGE_URL ">\n");
	printf("Bug Reports: <" PACKAGE_BUGREPORT ">\n");
}

static void parse_opts(int argc, char **argv)
{
	static struct option longopts[] = {
		{"help", no_argument, NULL, 'h'},
		{"foreground", no_argument, NULL, 'f'},
		{"verbose", no_argument, NULL, 'v'},
		{"verbose-usb", no_argument, NULL, 'w' },
		{"tcp", 0, no_argument, 't' },
		{"all-interfaces", 0, no_argument, 'a' },
		{"user", required_argument, NULL, 'U'},
		{"disable-hotplug", no_argument, NULL, 'n'},
		{"enable-exit", no_argument, NULL, 'z'},
		{"no-preflight", no_argument, NULL, 'p'},
#ifdef HAVE_UDEV
		{"udev", no_argument, NULL, 'u'},
#endif
#ifdef HAVE_SYSTEMD
		{"systemd", no_argument, NULL, 's'},
#endif
		{"exit", no_argument, NULL, 'x'},
		{"force-exit", no_argument, NULL, 'X'},
		{"logfile", required_argument, NULL, 'l'},
		{"version", no_argument, NULL, 'V'},
		{NULL, 0, NULL, 0}
	};
	int c;

#ifdef HAVE_SYSTEMD
	const char* opts_spec = "hfvVwtauU:xXsnzl:p";
#elif HAVE_UDEV
	const char* opts_spec = "hfvVwtauU:xXnzl:p";
#else
	const char* opts_spec = "hfvVwtaU:xXnzl:p";
#endif

	while (1) {
		c = getopt_long(argc, argv, opts_spec, longopts, (int *) 0);
		if (c == -1) {
			break;
		}

		switch (c) {
		case 'h':
			usage();
			exit(0);
		case 'f':
			foreground = 1;
			break;
		case 'v':
			++verbose;
			break;
		case 'w':
			libusb_verbose = 255;
			break;
		case 't':
			tcp = 1;
			break;
		case 'a':
			all_interfaces = 1;
			break;
		case 'V':
			printf("%s\n", PACKAGE_STRING);
			exit(0);
		case 'U':
			drop_privileges = 1;
			drop_user = optarg;
			break;
		case 'p':
			no_preflight = 1;
			break;
#ifdef HAVE_UDEV
		case 'u':
			opt_disable_hotplug = 1;
			opt_enable_exit = 1;
			break;
#endif
#ifdef HAVE_SYSTEMD
		case 's':
			opt_enable_exit = 1;
			foreground = 1;
			break;
#endif
		case 'n':
			opt_disable_hotplug = 1;
			break;
		case 'z':
			opt_enable_exit = 1;
			break;

#ifdef WIN32
#else
		case 'x':
			opt_exit = 1;
			exit_signal = SIGUSR1;
			break;
		case 'X':
			opt_exit = 1;
			exit_signal = SIGTERM;
			break;
#endif
		case 'l':
			if (!*optarg) {
				usbmuxd_log(LL_FATAL, "ERROR: --logfile requires a non-empty filename");
				usage();
				exit(2);
			}
			if (use_logfile) {
				usbmuxd_log(LL_FATAL, "ERROR: --logfile cannot be used multiple times");
				exit(2);
			}
			if (!freopen(optarg, "a", stderr)) {
				usbmuxd_log(LL_FATAL, "ERROR: fdreopen: %s", strerror(errno));
			} else {
				use_logfile = 1;
			}
			break;
		default:
			usage();
			exit(2);
		}
	}
}

int main(int argc, char *argv[])
{
	int listenfd;
	int res = 0;
	int lfd;
	char pids[10];

#ifdef WIN32
	HANDLE mutex;
#else
	struct flock lock;
#endif

	parse_opts(argc, argv);

	argc -= optind;
	argv += optind;

#ifdef WIN32
	verbose += LL_NOTICE;
#else
    if (!foreground && !use_logfile) {
		verbose += LL_WARNING;
#ifndef WIN32
		log_enable_syslog();
#endif
	} else {
		verbose += LL_NOTICE;
	}
#endif

	/* set log level to specified verbosity */
	log_level = verbose;

#ifdef HAVE_LIBIMOBILEDEVICE
	if (log_level == 0) {
		// No logging for libimobiledevice
	}
	else if (log_level == 1) {
		usbmuxd_log(LL_NOTICE, "enabling libimobiledevice logging");
		idevice_set_debug_level(1);
	}
	else {
		usbmuxd_log(LL_NOTICE, "enabling libimobiledevice logging");
		idevice_set_debug_level(2);
	}

	if (tcp) {
		idevice_set_socket_type(IDEVICE_SOCKET_TYPE_TCP);
	}
#endif

	usbmuxd_log(LL_NOTICE, "usbmuxd v%s starting up", PACKAGE_VERSION);
	should_exit = 0;
	should_discover = 0;

#ifdef WIN32
	mutex = CreateMutex(NULL, TRUE, "usbmuxd");
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		usbmuxd_log(LL_ERROR, "Another instance is already running. Exiting.");
		res = -1;
		goto terminate;
	}
#else
	set_signal_handlers();
	signal(SIGPIPE, SIG_IGN);

	res = lfd = open(lockfile, O_WRONLY|O_CREAT, 0644);
	if(res == -1) {
		usbmuxd_log(LL_FATAL, "Could not open lockfile");
		goto terminate;
	}
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = 0;
	lock.l_len = 0;
	lock.l_pid = 0;
	fcntl(lfd, F_GETLK, &lock);
	close(lfd);
	if (lock.l_type != F_UNLCK) {
		if (opt_exit) {
			if (lock.l_pid && !kill(lock.l_pid, 0)) {
				usbmuxd_log(LL_NOTICE, "Sending signal %d to instance with pid %d", exit_signal, lock.l_pid);
				res = 0;
				if (kill(lock.l_pid, exit_signal) < 0) {
					usbmuxd_log(LL_FATAL, "Could not deliver signal %d to pid %d", exit_signal, lock.l_pid);
					res = -1;
				}
				goto terminate;
			} else {
				usbmuxd_log(LL_ERROR, "Could not determine pid of the other running instance!");
				res = -1;
				goto terminate;
			}
		} else {
			if (!opt_disable_hotplug) {
				usbmuxd_log(LL_ERROR, "Another instance is already running (pid %d). exiting.", lock.l_pid);
				res = -1;
			} else {
				usbmuxd_log(LL_NOTICE, "Another instance is already running (pid %d). Telling it to check for devices.", lock.l_pid);
				if (lock.l_pid && !kill(lock.l_pid, 0)) {
					usbmuxd_log(LL_NOTICE, "Sending signal SIGUSR2 to instance with pid %d", lock.l_pid);
					res = 0;
					if (kill(lock.l_pid, SIGUSR2) < 0) {
						usbmuxd_log(LL_FATAL, "Could not deliver SIGUSR2 to pid %d", lock.l_pid);
						res = -1;
					}
				} else {
					usbmuxd_log(LL_ERROR, "Could not determine pid of the other running instance!");
					res = -1;
				}
			}
			goto terminate;
		}
	}
	unlink(lockfile);

	if (opt_exit) {
		usbmuxd_log(LL_NOTICE, "No running instance found, none killed. Exiting.");
		goto terminate;
	}

	if (!foreground) {
		if ((res = daemonize()) < 0) {
			fprintf(stderr, "usbmuxd: FATAL: Could not daemonize!\n");
			usbmuxd_log(LL_FATAL, "Could not daemonize!");
			goto terminate;
		}
	}

	// now open the lockfile and place the lock
	res = lfd = open(lockfile, O_WRONLY|O_CREAT|O_TRUNC|O_EXCL, 0644);
	if(res < 0) {
		usbmuxd_log(LL_FATAL, "Could not open lockfile");
		goto terminate;
	}
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = 0;
	lock.l_len = 0;
	if ((res = fcntl(lfd, F_SETLK, &lock)) < 0) {
		usbmuxd_log(LL_FATAL, "Lockfile locking failed!");
		goto terminate;
	}
	sprintf(pids, "%d", getpid());
	if ((size_t)(res = write(lfd, pids, strlen(pids))) != strlen(pids)) {
		usbmuxd_log(LL_FATAL, "Could not write pidfile!");
		if(res >= 0)
			res = -2;
		goto terminate;
	}

	// set number of file descriptors to higher value
	struct rlimit rlim;
	getrlimit(RLIMIT_NOFILE, &rlim);
	rlim.rlim_max = 65536;
	setrlimit(RLIMIT_NOFILE, (const struct rlimit*)&rlim);
#endif

	usbmuxd_log(LL_INFO, "Creating socket");
	res = listenfd = create_socket();
	if(listenfd < 0)
		goto terminate;

#ifdef HAVE_LIBIMOBILEDEVICE
	const char* userprefdir = config_get_config_dir();
	struct stat fst;
	memset(&fst, '\0', sizeof(struct stat));
	if (stat(userprefdir, &fst) < 0) {
#ifdef WIN32
		if (_mkdir(userprefdir) < 0) {
#else
		if (mkdir(userprefdir, 0775) < 0) {
#endif
			usbmuxd_log(LL_FATAL, "Failed to create required directory '%s': %s", userprefdir, strerror(errno));
			res = -1;
			goto terminate;
		}
		if (stat(userprefdir, &fst) < 0) {
			usbmuxd_log(LL_FATAL, "stat() failed after creating directory '%s': %s", userprefdir, strerror(errno));
			res = -1;
			goto terminate;
		}
	}

	// make sure permission bits are set correctly
#ifndef _MSC_VER
	if (fst.st_mode != 02775) {
		if (chmod(userprefdir, 02775) < 0) {
			usbmuxd_log(LL_WARNING, "chmod(%s, 02775) failed: %s", userprefdir, strerror(errno));
		}
	}
#endif
#endif

#ifdef WIN32
#else
	// drop elevated privileges
	if (drop_privileges && (getuid() == 0 || geteuid() == 0)) {
		struct passwd *pw;
		if (!drop_user) {
			usbmuxd_log(LL_FATAL, "No user to drop privileges to?");
			res = -1;
			goto terminate;
		}
		pw = getpwnam(drop_user);
		if (!pw) {
			usbmuxd_log(LL_FATAL, "Dropping privileges failed, check if user '%s' exists!", drop_user);
			res = -1;
			goto terminate;
		}
		if (pw->pw_uid == 0) {
			usbmuxd_log(LL_INFO, "Not dropping privileges to root");
		} else {
#ifdef HAVE_LIBIMOBILEDEVICE
			/* make sure the non-privileged user has proper access to the config directory */
			if ((fst.st_uid != pw->pw_uid) || (fst.st_gid != pw->pw_gid)) {
				if (chown(userprefdir, pw->pw_uid, pw->pw_gid) < 0) {
					usbmuxd_log(LL_WARNING, "chown(%s, %d, %d) failed: %s", userprefdir, pw->pw_uid, pw->pw_gid, strerror(errno));
				}
			}
#endif

			if ((res = initgroups(drop_user, pw->pw_gid)) < 0) {
				usbmuxd_log(LL_FATAL, "Failed to drop privileges (cannot set supplementary groups)");
				goto terminate;
			}
			if ((res = setgid(pw->pw_gid)) < 0) {
				usbmuxd_log(LL_FATAL, "Failed to drop privileges (cannot set group ID to %d)", pw->pw_gid);
				goto terminate;
			}
			if ((res = setuid(pw->pw_uid)) < 0) {
				usbmuxd_log(LL_FATAL, "Failed to drop privileges (cannot set user ID to %d)", pw->pw_uid);
				goto terminate;
			}

			// security check
			if (setuid(0) != -1) {
				usbmuxd_log(LL_FATAL, "Failed to drop privileges properly!");
				res = -1;
				goto terminate;
			}
			if (getuid() != pw->pw_uid || getgid() != pw->pw_gid) {
				usbmuxd_log(LL_FATAL, "Failed to drop privileges properly!");
				res = -1;
				goto terminate;
			}
			usbmuxd_log(LL_NOTICE, "Successfully dropped privileges to '%s'", drop_user);
		}
	}
#endif

	client_init();
	device_init();
	usbmuxd_log(LL_INFO, "Initializing USB");
	usb_set_log_level(libusb_verbose);
	if((res = usb_initialize()) < 0)
		goto terminate;

	usbmuxd_log(LL_INFO, "%d device%s detected", res, (res==1)?"":"s");

	usbmuxd_log(LL_NOTICE, "Initialization complete");

#ifdef WIN32
#else
	if (report_to_parent)
		if((res = notify_parent(0)) < 0)
			goto terminate;
#endif

	if(opt_disable_hotplug) {
		usbmuxd_log(LL_NOTICE, "Automatic device discovery on hotplug disabled.");
		usb_autodiscover(0); // discovery to be triggered by new instance
	}
	if (opt_enable_exit) {
		usbmuxd_log(LL_NOTICE, "Enabled exit on SIGUSR1 if no devices are attached. Start a new instance with \"--exit\" to trigger.");
	}

	res = main_loop(listenfd);
	if(res < 0)
		usbmuxd_log(LL_FATAL, "main_loop failed");

	usbmuxd_log(LL_NOTICE, "usbmuxd shutting down");
	device_kill_connections();
	usb_shutdown();
	device_shutdown();
	client_shutdown();
	usbmuxd_log(LL_NOTICE, "Shutdown complete");

terminate:
#ifdef WIN32
	ReleaseMutex(mutex);
#endif

#ifdef WIN32
#else
	log_disable_syslog();
#endif

	if (res < 0)
		res = -res;
	else
		res = 0;

#ifdef WIN32
#else
	if (report_to_parent)
		notify_parent(res);
#endif

	return res;
}
