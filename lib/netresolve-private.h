/* Copyright (c) 2013 Pavel Šimerda, Red Hat, Inc. (psimerda at redhat.com) and others
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <netresolve.h>
#include <netresolve-backend.h>
#include <netresolve-string.h>
#include <netresolve-cli.h>
#include <nss.h>
#include <netdb.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <assert.h>

enum netresolve_state {
	NETRESOLVE_STATE_INIT,
	NETRESOLVE_STATE_WAITING,
	NETRESOLVE_STATE_FINISHED,
	NETRESOLVE_STATE_FAILED
};

struct netresolve_backend {
	bool mandatory;
	char **settings;
	void *dl_handle;
	void (*start)(netresolve_t channel, char **settings);
	void (*dispatch)(netresolve_t channel, int fd, int revents);
	void (*cleanup)(netresolve_t channel);
	void *data;
};

struct netresolve_path {
	struct {
		int family;
		union {
			char address[1024];
			struct in_addr address4;
			struct in6_addr address6;
		};
		int ifindex;
	} node;
	struct {
		int socktype;
		int protocol;
		int port;
	} service;
	int priority;
	int weight;
	int ttl;
	struct {
		enum netresolve_state state;
		int fd;
	} socket;
};

struct netresolve_channel {
	int log_level;
	enum netresolve_state state;
	int epoll_fd;
	int epoll_count;
	char *backend_string;
	struct netresolve_backend **backends, **backend;
	int first_connect_timeout;
	struct {
		netresolve_callback_t on_success;
		netresolve_callback_t on_failure;
		void *user_data;
		netresolve_fd_callback_t watch_fd;
		void *user_data_fd;
		netresolve_socket_callback_t on_bind;
		netresolve_socket_callback_t on_connect;
		void *user_data_sock;
	} callbacks;
	struct netresolve_request {
		/* Perform L3 address resolution using 'node' if not NULL. Use
		 * 'family' to chose between IPv4, IPv6 and mixed IPv4/IPv6
		 * resolution and additional flags to further tweak node name
		 * resolution.
		 */
		const char *node;
		int family;
		/* Perform L4 port resolution using 'service' if not NULL. Use
		 * 'socktype' and 'protocol' to limit the possible options and
		 * additional flags to further tweak service name resolution.
		 */
		const char *service;
		int socktype;
		int protocol;
		/* Advanced configuration */
		bool default_loopback;
		bool dns_srv_lookup;
	} request;
	struct netresolve_response {
		struct netresolve_path *paths;
		size_t pathcount;
		char *canonname;
	} response;

	union {
		struct sockaddr sa;
		struct sockaddr_in sin;
		struct sockaddr_in6 sin6;
	} sa_buffer;
	char buffer[1024];
};

void netresolve_set_state(netresolve_t channel, enum netresolve_state state);

void netresolve_start(netresolve_t channel);
void netresolve_epoll(netresolve_t channel, int timeout);
void netresolve_watch_fd(netresolve_t channel, int fd, int events);
int netresolve_add_timeout(netresolve_t channel, time_t sec, long nsec);
void netresolve_remove_timeout(netresolve_t channel, int fd);

void netresolve_bind_path(netresolve_t channel, struct netresolve_path *path);
void netresolve_connect_start(netresolve_t channel);
bool netresolve_connect_dispatch(netresolve_t channel, int fd, int events);
void netresolve_connect_cleanup(netresolve_t channel);

void netresolve_get_service_info(void (*callback)(int, int, int, void *), void *user_data,
		const char *request_service, int socktype, int protocol);
