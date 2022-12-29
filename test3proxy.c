/**
 * Run program without arguments to see usage.
 * 
 * The program imitates HTTP/2 traffic.
 *
 * In server mode
 * - accept a single client
 * - consume one dummy "request"
 * - send dummy "response"
 * - send "go away"
 * - wait for EOF from client
 *
 * In client mode
 * - connect to server
 * - send one dummy "request"
 * - keep sending more "requests"
 * - consume all data from server, look for "go away"
 * - wait for EOF from server
 *
 * Optionally connect via HTTP CONNECT proxy
 *
 */

#include <sys/types.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define SHUT_RDWR SD_BOTH
#define socklen_t int
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

#include <errno.h>

#include <signal.h>

#include <time.h>

#define OK(what) do { if (!(what)) { perror(#what); exit(1); } } while (0)
#define SOK OK

#ifndef timespecsub
#define timespecsub(tsp, usp, vsp)                                      \
        do {                                                            \
                (vsp)->tv_sec = (tsp)->tv_sec - (usp)->tv_sec;          \
                (vsp)->tv_nsec = (tsp)->tv_nsec - (usp)->tv_nsec;       \
                if ((vsp)->tv_nsec < 0) {                               \
                        (vsp)->tv_sec--;                                \
                        (vsp)->tv_nsec += 1000000000L;                  \
                }                                                       \
        } while (0)
#endif

#ifndef TIMESPEC_TO_TIMEVAL
#define TIMESPEC_TO_TIMEVAL(tv, ts) {                                   \
        (tv)->tv_sec = (ts)->tv_sec;                                    \
        (tv)->tv_usec = (ts)->tv_nsec / 1000;                           \
}
#endif


static void failusage(int argc, char *argv[]) {
	fprintf(stderr, "Usage: %s {client|server} [[hostaddr][:port]] [proxyaddr:port]\n", argc > 0 ? argv[0] : "program");
	exit(1);
}

#define SKIP_BYTES  10000
#define EXTRA_BYTES 50000
#define BUFSZ 2000

static int servermode;

static char *server_host = NULL;

static char *server_port = NULL;

static char *proxy_host = NULL;

static char *proxy_port = NULL;

static char buf[BUFSZ] = { 0 };

static SOCKET s;

static inline int minint(int a, int b) {
	return a < b ? a : b;
}

static const char *ltr;

static struct addrinfo *my_getaddrinfo(const char *node, const char *service)
{
  struct addrinfo hints, *res;
  int val;

  memset (&hints, 0, sizeof (hints));
  hints.ai_socktype = SOCK_STREAM;

  val = getaddrinfo (node, service, &hints, &res);
  if (val != 0) {
    fprintf(stderr, "%s getaddrinfo: %s\n", ltr, gai_strerror(val));
    exit(EXIT_FAILURE);
  }
  return res;
}

static void my_printaddr(struct sockaddr *addr, socklen_t addrlen) {
  int val;
  char host[1024], service[1024];
  val = getnameinfo(addr, addrlen, host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
  if (0 != val) {
    fprintf(stderr, "%s getnameinfo: %s\n", ltr, gai_strerror(val));
    exit(EXIT_FAILURE);
  }

  printf("%s %s address: %s:%s\n", ltr, (proxy_host ? "Proxy" : "Server"), host, service);
}


#ifdef _WIN32
#define my_send_nosigpipe send
#else
static
ssize_t my_send_nosigpipe(SOCKET fd, const void *buf, size_t len, int flags)
{
    int save_errno;
    sigset_t oldset, newset;
    ssize_t result;
    siginfo_t si;
    struct timespec ts = {0};

    sigemptyset(&newset);
    sigaddset(&newset, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &newset, &oldset);

    result = send(fd, buf, len, flags);
    save_errno = errno;

    while (sigtimedwait(&newset, &si, &ts)>=0 || errno != EAGAIN);
    pthread_sigmask(SIG_SETMASK, &oldset, 0);

    errno = save_errno;
    return result;
}
#endif /* _WIN32 */

static void my_send(int remaining) {
	ssize_t nb;
	printf("%s Sending %d bytes...\n", ltr, remaining);
	for (;remaining != 0;) {
		SOK(SOCKET_ERROR != (nb = my_send_nosigpipe(s, buf, minint(remaining, BUFSZ), 0)));
		remaining -= nb;
	}
	printf("%s Done\n", ltr);
}

static void my_skip(int remaining) {
	ssize_t nb;
	printf("%s Receiving %d bytes...\n", ltr, remaining);
	for (; remaining != 0;) {
		SOK(SOCKET_ERROR != (nb = recv(s, buf, minint(remaining, BUFSZ), 0)));
		if (nb == 0) {
			fprintf(stderr, "%s Connection closed before first request consumed\n", ltr);
			exit(1);
		}
		remaining -= nb;
	}
	printf("%s Done\n", ltr);
}

static void my_send_extra_requests() {
	printf("%s Sending extra requests...\n", ltr);
	ssize_t nb;
	for (;;) {
		fd_set readfds;
		fd_set writefds;

		FD_ZERO(&readfds);
		FD_SET(s, &readfds);
		FD_ZERO(&writefds);
		FD_SET(s, &writefds);

		int selectedfds;
		SOK(SOCKET_ERROR != (selectedfds = select(s + 1, &readfds, &writefds, NULL, NULL)));

		if (selectedfds != 0) {
			if (FD_ISSET(s, &readfds)) {
				SOK(SOCKET_ERROR != (nb = recv(s, buf, BUFSZ, 0)));
				if (nb == 0) {
					fprintf(stderr, "%s Connection closed before go away received\n", ltr);
					exit(1);
				}
				for (ssize_t i = 0; i != nb; i++) {
					if (buf[i]) {
						printf("%s Received go away\n", ltr);
						memset(buf, 0, BUFSZ);
						return;
					}
				}
			}
			
			if (FD_ISSET(s, &writefds)) {
				SOK(SOCKET_ERROR != (nb = my_send_nosigpipe(s, buf, BUFSZ, 0)));
			}
		}
	}
}

#define EOF_SECONDS 5

static void my_skip_with_timeout() {
	printf("%s Waiting %d seconds for EOF...\n", ltr, EOF_SECONDS);
	ssize_t nb;
	struct timespec deadline;
	OK(0 == clock_gettime(CLOCK_MONOTONIC, &deadline));
	deadline.tv_sec += EOF_SECONDS;
	for (;;) {

		struct timespec now, difference;
		OK(0 == clock_gettime(CLOCK_MONOTONIC, &now));

		struct timeval timeout = { 0 };

		timespecsub(&deadline, &now, &difference);

		if (difference.tv_sec < 0) {
			printf("%s Timeout\n", ltr);
			break;
		}

		TIMESPEC_TO_TIMEVAL(&timeout, &difference);

		fd_set readfds;

		FD_ZERO(&readfds);
		FD_SET(s, &readfds);

		int selectedfds;
		SOK(SOCKET_ERROR != (selectedfds = select(s + 1, &readfds, NULL, NULL, &timeout)));

		if (selectedfds != 0) {
			if (FD_ISSET(s, &readfds)) {
				SOK(SOCKET_ERROR != (nb = recv(s, buf, BUFSZ, 0)));
				if (nb == 0) {
					printf("%s Received EOF\n", ltr);
					break;
				}
			}
		}
	}
}

static void my_linger() {
	struct linger linger = {0};
	socklen_t optlen = sizeof(linger);
	linger.l_onoff = 1;
	linger.l_linger = 5;
	SOK(0 == setsockopt(s, SOL_SOCKET, SO_LINGER, &linger, optlen));
	//
	linger.l_onoff = 0;
	linger.l_linger = 0;
	//
	SOK(0 == getsockopt(s, SOL_SOCKET, SO_LINGER, &linger, &optlen));
	printf("%s linger: {%d,%d}\n", ltr, linger.l_onoff, linger.l_linger);
}

static void my_parse_host_and_port(const char *host_and_port, char **pp_server_host, char **pp_server_port) {
	const char *p_last_colon = strrchr(host_and_port, ':');
	if (p_last_colon && !strchr(p_last_colon, ']')) {
		// host and port
		if (p_last_colon != host_and_port) {
			*pp_server_host = strndup(host_and_port, p_last_colon - host_and_port);
		}
		*pp_server_port = strdup(p_last_colon + 1);
	} else {
		// no port
		*pp_server_host = strdup(host_and_port);
	}
}

static void my_check_200() {
	int n = -1;
	sscanf(buf, "%*[^ \n]%*1[ ]200%*1[ ]%n", &n);
	if (n == -1) {
		fprintf(stderr, "%s Unexpected HTTP status\n", ltr);
		exit(1);
	}
}

static void my_print_http(const char *pref) {
	char *nexttoken = buf;
	for(;;) {
		const char *line = strsep(&nexttoken, "\n");
		if (nexttoken || line[0]) {
			printf("%s %s %s\n", ltr, pref, line);
		}
		if (!nexttoken) {
			break;
		}
		nexttoken[-1] = '\n';
	}
}

int
main (int argc, char *argv[])
{
	if (argc < 2) {
		failusage(argc, argv);
	}
	if (0 == strcmp(argv[1], "client")) {
		servermode = 0;
		ltr = "C";
	} else if (0 == strcmp(argv[1], "server")) {
		servermode = 1;
		ltr = "S";
	} else {
		failusage(argc, argv);
	}

	if (argc >= 3) {
		my_parse_host_and_port(argv[2], &server_host, &server_port);
	}
	if (!server_host) {
		server_host = strdup(servermode ? "0.0.0.0" : "127.0.0.1");
	}
	if (!server_port) {
		server_port = strdup("9999");
	}

	if (argc >= 4) {
		if (servermode) {
			failusage(argc, argv);
		}
		my_parse_host_and_port(argv[3], &proxy_host, &proxy_port);
		if (!proxy_host || !proxy_port) {
			failusage(argc, argv);
		}
	}

	struct addrinfo *ptr= proxy_host ? my_getaddrinfo(proxy_host, proxy_port) : my_getaddrinfo(server_host, server_port);
	my_printaddr(ptr->ai_addr, ptr->ai_addrlen);

	//ssize_t nb;

	if (servermode) {
		SOCKET listen_socket;
		SOK(INVALID_SOCKET != (listen_socket = socket(ptr->ai_family, SOCK_STREAM, IPPROTO_TCP)));

		int reuse = 1;
		socklen_t optlen = sizeof(reuse);
		SOK(0 == setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, optlen));

		SOK(0 == bind(listen_socket, ptr->ai_addr, ptr->ai_addrlen));
		SOK(0 == listen(listen_socket, SOMAXCONN));
		printf("%s Listening...\n", ltr);
		SOK(INVALID_SOCKET != (s = accept(listen_socket, NULL, NULL)));
		printf("%s Accepted\n", ltr);
		my_linger();

		my_skip(SKIP_BYTES);

		my_send(EXTRA_BYTES*10);
		buf[0] = 1;
		my_send(1); // go away!
		buf[0] = 0;
		
		printf("%s Shutting down...\n", ltr);
		SOK(0 == shutdown(s, SHUT_WR));
		
		my_skip_with_timeout();

		printf("%s Closing...\n", ltr);
		SOK(0 == closesocket(s));

	} else {
		// client
		SOK(INVALID_SOCKET != (s = socket(ptr->ai_family, SOCK_STREAM, IPPROTO_TCP)));
		printf("%s Connecting...\n", ltr);
		SOK(0 == connect(s, ptr->ai_addr, ptr->ai_addrlen));
		printf("%s Connected\n", ltr);
		
		my_linger();

		if (proxy_host) {
			int port;
			{
				struct addrinfo hints, *res;
				int val;

				memset (&hints, 0, sizeof (hints));
				hints.ai_socktype = SOCK_STREAM;
				hints.ai_protocol = IPPROTO_TCP;

				val = getaddrinfo (NULL, server_port, &hints, &res);
				if (val != 0) {
					fprintf(stderr, "%s getaddrinfo: %s\n", ltr, gai_strerror(val));
					exit(EXIT_FAILURE);
				}
				if (AF_INET6 != res->ai_family && AF_INET != res->ai_family) {
					fprintf(stderr, "%s Unexpected address family of port\n", ltr);
					exit(EXIT_FAILURE);
				}
				port = ntohs( ((struct sockaddr_in*)res->ai_addr)->sin_port );
			}
			printf("%s Sending proxy request...\n", ltr);
			ssize_t nb = snprintf(buf, BUFSZ,
					"CONNECT %1$s:%2$d HTTP/1.1\r\n"
					"Host: %1$s:%2$d\r\n"
					"User-Agent: curl/7.84.0\r\n"
					"Proxy-Connection: Keep-Alive\r\n"
					"\r\n"
					, server_host, port);
			if (nb >= BUFSZ) {
				fprintf(stderr, "%s Proxy request won't fit into buffer\n", ltr);
				exit(1);
			}
			my_print_http(">");
			my_send(nb);
			printf("%s Receiving proxy response\n", ltr);
			// \n\r\n or \n\n
			int i = 0;
			for (;;i++) {
				if (i >= BUFSZ - 1) {
					fprintf(stderr, "%s Proxy response won't fit into buffer\n", ltr);
					exit(1);
				}
				SOK(SOCKET_ERROR != (nb = recv(s, buf + i, 1, 0)));
				if (nb == 0) {
					fprintf(stderr, "%s Proxy response could not be parsed\n", ltr);
					exit(1);
				}
				char c = *(buf + i);
				if (c == '\n' && ( (i >= 2 && '\r' == buf[i-1] && '\n' == buf[i-2]) || (i >= 1 && '\n' == buf[i-1])  ) ) {
					buf[i+1] = 0;
					break;
				}
			}
			my_print_http("<");
			my_check_200();
			memset(buf, 0, BUFSZ);
		}
		
		my_send(SKIP_BYTES);
		
		my_send_extra_requests();

		printf("%s Shutting down...\n", ltr);
		SOK(0 == shutdown(s, SHUT_WR));

		my_skip_with_timeout();

		printf("%s Closing...\n", ltr);
		SOK(0 == closesocket(s));
	}

	return 0;
}
//
