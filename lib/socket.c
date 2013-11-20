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
#include <unistd.h>

#include "netresolve-private.h"

#define FIRST_CONNECT_TIMEOUT 1

void
netresolve_bind_path(netresolve_query_t query, struct netresolve_path *path)
{
	int flags = O_NONBLOCK;
	int socktype;
	int protocol;
	const struct sockaddr *sa;
	socklen_t salen;
	int idx = path - query->response.paths;
	int sock;

	sa = netresolve_query_get_path_sockaddr(query, idx, &salen, &socktype, &protocol);
	if (!sa)
		return;
	sock = socket(sa->sa_family, socktype | flags, protocol);
	if (sock == -1)
		return;
	if (bind(sock, sa, salen) == -1) {
		close(sock);
		return;
	}

	query->callbacks.on_bind(query, idx, sock, query->callbacks.user_data_sock);
}

static void
connect_path(netresolve_query_t query, struct netresolve_path *path)
{
	static const int flags = O_NONBLOCK;
	int socktype;
	int protocol;
	const struct sockaddr *sa;
	socklen_t salen;

	if (path->socket.state != NETRESOLVE_STATE_INIT)
		return;

	sa = netresolve_query_get_path_sockaddr(query, path - query->response.paths, &salen, &socktype, &protocol);
	if (!sa)
		goto fail;
	path->socket.fd = socket(sa->sa_family, socktype | flags, protocol);
	if (path->socket.fd == -1)
		goto fail;
	if (connect(path->socket.fd, sa, salen) == -1 && errno != EINPROGRESS)
		goto fail_connect;

	netresolve_watch_fd(query, path->socket.fd, POLLOUT);
	path->socket.state = NETRESOLVE_STATE_WAITING;
	return;

fail_connect:
	close(path->socket.fd);
	path->socket.fd = -1;
fail:
	path->socket.state = NETRESOLVE_STATE_FAILED;
}

static void
connect_check(netresolve_query_t query)
{
	int idx;

	for (idx = 0; idx < query->response.pathcount; idx++) {
		struct netresolve_path *path = &query->response.paths[idx];

		if (path->socket.state < NETRESOLVE_STATE_FINISHED)
			break;

		if (path->socket.state == NETRESOLVE_STATE_FINISHED) {
			query->callbacks.on_connect(query, idx, path->socket.fd, query->callbacks.user_data_sock);
			path->socket.state = NETRESOLVE_STATE_INIT;
			netresolve_set_state(query, NETRESOLVE_STATE_FINISHED);
			break;
		}
	}
}

static void
connect_finished(netresolve_query_t query, struct netresolve_path *path)
{
	path->socket.state = NETRESOLVE_STATE_FINISHED;

	if (query->first_connect_timeout == -1)
		query->first_connect_timeout = netresolve_add_timeout(query, FIRST_CONNECT_TIMEOUT, 0);

	connect_check(query);
}

static void
connect_failed(netresolve_query_t query, struct netresolve_path *path)
{
	int family = path->node.family;

	path->socket.state = NETRESOLVE_STATE_FAILED;
	close(path->socket.fd);

	while (path < query->response.paths + query->response.pathcount)
		if (path->node.family == family)
			connect_path(query, path);

	connect_check(query);
}

void
netresolve_connect_start(netresolve_query_t query)
{
	bool ip4 = false;
	bool ip6 = false;
	int i;

	debug("socket: connecting...\n");

	for (i = 0; i < query->response.pathcount; i++) {
		struct netresolve_path *path = &query->response.paths[i];

		if (!ip4 && path->node.family == AF_INET && path->socket.state == NETRESOLVE_STATE_INIT) {
			connect_path(query, path);
			ip4 = true;
		}
		if (!ip6 && path->node.family == AF_INET6 && path->socket.state == NETRESOLVE_STATE_INIT) {
			connect_path(query, path);
			ip6 = true;
		}
	}
}

bool
netresolve_connect_dispatch(netresolve_query_t query, int fd, int events)
{
	int i;

	debug("socket: dispatching file descriptor: %d %d\n", fd, events);

	for (i = 0; i < query->response.pathcount; i++) {
		struct netresolve_path *path = &query->response.paths[i];

		if (fd == path->socket.fd) {
			netresolve_watch_fd(query, path->socket.fd, 0);

			if (events & POLLOUT) {
				socklen_t len = sizeof(errno);

				getsockopt(path->socket.fd, SOL_SOCKET, SO_ERROR, &errno, &len);

				if (errno)
					connect_failed(query, path);
				else
					connect_finished(query, path);
			}

			return true;
		}
	}

	if (fd == query->first_connect_timeout) {
		for (i = 0; i < query->response.pathcount; i++) {
			struct netresolve_path *path = &query->response.paths[i];

			switch (path->socket.state) {
			case NETRESOLVE_STATE_WAITING:
				close(path->socket.fd);
				/* path through */
			case NETRESOLVE_STATE_INIT:
				path->socket.state = NETRESOLVE_STATE_FAILED;
				break;
			default:
				break;
			}
		}

		connect_check(query);
		return true;
	}

	return false;
}

void
netresolve_connect_cleanup(netresolve_query_t query)
{
	int i;

	debug("socket: cleaning up...\n");

	for (i = 0; i < query->response.pathcount; i++) {
		struct netresolve_path *path = &query->response.paths[i];

		switch (path->socket.state) {
		case NETRESOLVE_STATE_WAITING:
		case NETRESOLVE_STATE_FINISHED:
			close(path->socket.fd);
			/* pass through */
		default:
			memset(&path->socket, 0, sizeof path->socket);
		}
	}

	if (query->first_connect_timeout != -1) {
		netresolve_remove_timeout(query, query->first_connect_timeout);
		query->first_connect_timeout = -1;
	}
}
