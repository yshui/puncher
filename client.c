#include <stdio.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <assert.h>
#include <ev.h>
#include <fcntl.h>
#include <stdint.h>

#include "common.h"
#include "uthash.h"

static struct sockaddr_in6 server_addr;

struct connection;

struct transfer {
	ev_io w;
	struct connection *c;
};

struct local {
	ev_io w;
	struct connection *c;
};

struct control;

struct connection {
	ev_io w;
	uint32_t id;
	int t_fd;
	int l_fd;
	struct sockaddr_in6 *taddr;
	int connected; // 0=no, 1=connecting, 2=connected
	struct transfer *t;
	struct local *l;
	struct control *c;
	UT_hash_handle hh;
};

struct request {
	uint32_t seq;
	struct connection *co;
	UT_hash_handle hh;
};

struct control {
	ev_io w;
	int ctrl_fd;
	struct connection *conns;
	struct request *pending_req;
};

static int lport, rport;

void transfer_cb(EV_P_ ev_io *w, int revent){
	fprintf(stderr, "remote port send data\n");
	struct transfer *t = (struct transfer *)w;
	if (t->c->connected == 0) {
		//This is impossible, just read the data and ignore it
		fprintf(stderr, "receive data from transfer before connect, impossible\n");
		char buf[4096];
		recv(t->c->t_fd, buf, 4096, 0);
		return;
	}else if (t->c->connected == 1) {
		fprintf(stderr, "receive data when connecting, wait\n");
		//Still connecting, so just return without reading the data
		//And stop the event.
		ev_io_stop(EV_A_ w);
		return;
	}

	char buf[4096];
	int ret = recv(t->c->t_fd, buf, 4096, 0);
	if (ret < 0) {
		perror("transfer_cb");
		return;
	}
	if (ret == 0) {
		fprintf(stderr, "Transfer port stoped without notice\n");
		ev_io_stop(EV_A_ w);
	}
	send(t->c->l_fd, buf, ret, 0);
}

void local_cb(EV_P_ ev_io *w, int revent){
	fprintf(stderr, "Local port send data\n");
	struct local *l = (struct local *)w;
	char buf[4096];
	int ret = recv(l->c->l_fd, buf, 4096, 0);
	if (ret < 0) {
		perror("Local port err");
		return;
	}
	fprintf(stderr, "Data length %u\n", ret);

	if (ret == 0) {
		struct control_packet cp;
		cp.action = CLOSE;
		cp.value = htonl(l->c->id);
		cp.seq = random();
		send(l->c->c->ctrl_fd, &cp, sizeof(cp), 0);
		shutdown(w->fd, SHUT_RDWR);
		shutdown(l->c->t_fd, SHUT_RDWR);
		ev_io_stop(EV_A_ (ev_io *)l->c->t);
		HASH_DEL(l->c->c->conns, l->c);
		ev_io_stop(EV_A_ w);
		free(l->c->t);
		free(l->c);
		free(l);
		return;
	}
	send(l->c->t_fd, buf, ret, 0);
}

void local_connect_cb(EV_P_ ev_io *w, int revent){
	fprintf(stderr, "Local port connected\n");
	struct local *l = (struct local *)w;
	l->c->connected = 2;

	if (!ev_is_active((ev_io *)l->c->t))
		ev_io_start(EV_A_ (ev_io *)l->c->t);

	ev_io_stop(EV_A_ w);
	ev_io_set(w, w->fd, EV_READ);
	ev_set_cb(w, local_cb);
	ev_io_start(EV_A_ w);
}

void control_cb(EV_P_ ev_io *w, int revent){
	if (revent != EV_READ)
		return;

	struct control *c = (struct control *)w;
	struct control_packet cp, res;
	struct sockaddr_in6 addr;
	struct connection *co;
	struct request *re, *nre = NULL;
	int ret = recv(c->ctrl_fd, &cp, sizeof(cp), 0);
	uint16_t port;
	uint32_t seq = ntohl(cp.seq);
	uint32_t value = ntohl(cp.value);
	if (ret < 0)
		return;
	if (ret == 0)
		ev_break(EV_A_ EVBREAK_ALL);
	memcpy(&res, &cp, sizeof(cp));


	switch(cp.action) {
		case OPEN_TCP:
			fprintf(stderr, "OPEN_TCP reply\n");
			co = talloc(1, struct connection);
			co->c = c;
			co->id = ntohl(cp.value);
			HASH_ADD_INT(c->conns, id, co);

			re = talloc(1, struct request);
			re->co = co;
			do {
				re->seq = random();
				HASH_FIND_INT(c->pending_req, &re->seq, nre);
			}while(nre);
			HASH_ADD_INT(c->pending_req, seq, re);
			res.action = TRANSFER_PORT;
			res.seq = htonl(re->seq);
			fprintf(stderr, "Sending TRANSFER_PORT with seq %u\n", res.seq);
			send(c->ctrl_fd, &res, sizeof(res), 0);
			break;
		case TRANSFER_PORT:
			fprintf(stderr, "TRANSFER_PORT reply\n");
			HASH_FIND_INT(c->pending_req, &seq, re);
			if (!re) {
				fprintf(stderr, "Response to non-existent seq %u\n", seq);
				break;
			}
			co = re->co;
			HASH_DEL(c->pending_req, re);
			free(re);
			port = ntohl(cp.value);

			struct sockaddr_in6 *taddr = talloc(1, struct sockaddr_in6);
			memcpy(taddr, &server_addr, sizeof(server_addr));
			taddr->sin6_port = port;
			int t_fd = socket(AF_INET6, SOCK_STREAM, 0);
			fcntl(t_fd, F_SETFL, O_NONBLOCK);
			connect(t_fd, (struct sockaddr *)taddr, sizeof(*taddr));
			struct transfer *t = talloc(1, struct transfer);
			t->c = co;
			co->t_fd = t_fd;
			co->taddr = taddr;
			co->t = t;

			ev_io_init((ev_io *)t, transfer_cb, t_fd, EV_READ);
			ev_io_start(EV_A_ (ev_io *)t);
			break;
		case TCP_CONNECTED:
			fprintf(stderr, "remote port connected\n");
			//Connect to the client as well
			HASH_FIND_INT(c->conns, &value, co);
			inet_pton(AF_INET6, "::1", &addr.sin6_addr);
			addr.sin6_family = AF_INET6;
			addr.sin6_port = lport;
			int l_fd = socket(AF_INET6, SOCK_STREAM, 0);
			fcntl(l_fd, F_SETFL, O_NONBLOCK);
			connect(l_fd, (struct sockaddr *)&addr, sizeof(addr));
			co->connected = 1;
			co->l_fd = l_fd;

			struct local *l = talloc(1, struct local);
			co->l = l;
			l->c = co;
			ev_io_init((ev_io *)l, local_connect_cb, l_fd, EV_WRITE);
			ev_io_start(EV_A_ (ev_io *)l);

			//Send a new OPEN_TCP so we can accept new connections
			res.action = OPEN_TCP;
			res.seq = random();
			res.value = htonl(rport);
			send(c->ctrl_fd, &res, sizeof(res), 0);
			break;
		case TCP_CLOSED:
			fprintf(stderr, "remote port closed\n");
			//Close this side as well
			HASH_FIND_INT(c->conns, &value, co);
			shutdown(co->l_fd, SHUT_RDWR);
			shutdown(co->t_fd, SHUT_RDWR);
			ev_io_stop(EV_A_ (ev_io *)co->t);
			HASH_DEL(c->conns, co);
			//Send CLOSE
			res.action = CLOSE;
			res.seq = random();
			res.value = htonl(co->id);
			send(c->ctrl_fd, &res, sizeof(res), 0);
			free(co);
	}
}

void server_connect_cb(EV_P_ ev_io *w, int revent){
	//Send OPEN_TCP
	struct control *c = (struct control *)w;
	struct control_packet cp;
	cp.action = OPEN_TCP;
	cp.seq = random();
	cp.value = htonl(rport);
	send(c->ctrl_fd, &cp, sizeof(cp), 0);

	ev_io_stop(EV_A_ w);
	ev_io_set(w, c->ctrl_fd, EV_READ);
	ev_set_cb(w, control_cb);
	ev_io_start(EV_A_ w);
}

void start_server_connect(EV_P){
	//Use the first address returned
	int fd = socket(AF_INET6, SOCK_STREAM, 0);
	fcntl(fd, F_SETFL, O_NONBLOCK);
	connect(fd, (struct sockaddr *)&server_addr, sizeof(server_addr));

	struct control *c = talloc(1, struct control);
	c->ctrl_fd = fd;
	ev_io_init((ev_io *)c, server_connect_cb, fd, EV_WRITE);
	ev_io_start(EV_A_ (ev_io *)c);
}

int main(int argc, const char **argv){
	if (argc < 6) {
		fprintf(stderr, "Usage: %s <t|u> local_port remote_port "
				"<t|u> server_ip server_port\n", argv[0]);
		return 1;
	}

	int rfd = open("/dev/random", O_RDONLY);
	uint32_t seed;
	read(rfd, &seed, sizeof(seed));
	close(rfd);

	srandom(seed);
	assert(argv[4][0] == 't');
	assert(argv[1][0] == 't');
	lport = htons(atoi(argv[2]));
	rport = htons(atoi(argv[3]));
	printf("%u %u %u %u\n", lport, rport, htonl(rport), ntohl(htonl(rport)));
	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
	hints.ai_socktype = argv[4][0] == 't' ? SOCK_STREAM : SOCK_DGRAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;          /* Any protocol */

	int ret = getaddrinfo(argv[5], argv[6], &hints, &res);
	if (ret != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
		exit(EXIT_FAILURE);
	}

	if (res->ai_family == AF_INET) {
		char tmp[16];
		struct sockaddr_in *tmpaddr = (struct sockaddr_in *)res->ai_addr;
		inet_ntop(AF_INET, &tmpaddr->sin_addr, tmp, res->ai_addrlen);
		char tmpv6[25];
		sprintf(tmpv6, "::ffff:%s", tmp);
		inet_pton(AF_INET6, tmpv6, &server_addr.sin6_addr);
	}else
		memcpy(&server_addr, res->ai_addr, sizeof(server_addr));

	server_addr.sin6_family = AF_INET6;
	server_addr.sin6_port = htons(atoi(argv[6]));
	free(res);
	start_server_connect(EV_DEFAULT);
	ev_run(EV_DEFAULT, 0);

	return 0;
}
