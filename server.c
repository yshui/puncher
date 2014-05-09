#define _GNU_SOURCE

#include <ev.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "common.h"
#include "uthash.h"

int setup_socket(const struct sockaddr_in6 *addr, int type){
	int fd = socket(AF_INET6, type == TCP ? SOCK_STREAM : SOCK_DGRAM, 0);
	bind(fd, (const struct sockaddr *)addr, sizeof(struct sockaddr_in6));
	fcntl(fd, F_SETFL, O_NONBLOCK);
	if (type == TCP)
		listen(fd, 1);
	return fd;
}

struct src_tuple {
	uint32_t id;
	struct sockaddr_in6 addr;
	UT_hash_handle hh;
	UT_hash_handle hh2;
};

struct connection {
	uint32_t seq;
	int ctrl_fd;
	int t_type, out_type;
	int t_fd, out_fd;
	int established;
	struct src_tuple *addr_map, *id_map;
	struct conn_event *ev;
	int max_udpid;
	UT_hash_handle hh;
};

struct conn_event {
	ev_io w;
	EV_CB_DECLARE(ev_io)
	void (*user_cb)(EV_P_ struct conn_event *, int);
	struct connection *c;
};

struct control_event {
	ev_io w;
	int type;
	struct sockaddr_in6 addr;
	struct connection *conns;
};

struct delay_send_event {
	ev_io w;
	char *data;
	struct connection *c;
};

void world_listen_cb(EV_P_ struct conn_event *ce, int revent){
	//Notify the client of tcp connection
	struct control_packet cp;
	cp.action = TCP_CONNECTED;
	cp.seq = random();
	cp.value = htonl(ce->c->seq);
	send(ce->c->ctrl_fd, &cp, sizeof(cp), 0);

	ce->c->out_fd = ce->w.fd;
}

void transfer_listen_cb(EV_P_ struct conn_event *ce, int revent){
	ce->c->established = 1;
	ce->c->t_fd = ce->w.fd;
	if (ce->c->ev) {
		ev_io_start(EV_A_ (ev_io *)ce->c->ev);
		ce->c->ev = NULL;
	}
}

void world_cb(EV_P_ ev_io *w, int revent){
	fprintf(stderr, "world port receive data\n");
	struct conn_event *ce = (struct conn_event *)w;
	struct sockaddr_in6 addr;
	socklen_t addrlen = sizeof(addr);
	char buf[2], *recvbuf;
	char *sendbuf = buf;
	//Peek first
	ssize_t ret;
	size_t sendsize;
	if (ce->c->out_type == UDP) {
		ret = recvfrom(ce->w.fd, buf, 2, MSG_DONTWAIT|MSG_PEEK|MSG_TRUNC,
			       &addr, &addrlen);
		if (ret < 0)
			return;

		recvbuf = malloc(ret);
		sendsize = ret;
		ret = recvfrom(ce->w.fd, recvbuf, ret, MSG_DONTWAIT|MSG_TRUNC,
			       NULL, NULL);
	}else{
		recvbuf = malloc(4096);
		ret = recv(ce->w.fd, recvbuf, 4096, MSG_DONTWAIT);
		sendsize = ret;
	}

	if (ret == 0) {
		//The connection has been closed
		if (ce->c->out_type == TCP) {
			struct control_packet cp;
			cp.action = TCP_CLOSED;
			cp.seq = random();
			cp.value = htonl(ce->c->seq);
			send(ce->c->ctrl_fd, &cp, sizeof(cp, 0), 0);
		}
		ev_io_stop(EV_A_ w);
		return;
	}

	if (!ce->c->established) {
		//The transfer connection hasn't established yet, ignore.
		fprintf(stderr, "Receive data when transfer hasn't been established\n");
		ev_io_stop(EV_A_ w);
		ce->c->ev = ce;
		return;
	}

	if (ce->c->out_type == UDP) {
		//Encode udp package for transfer
		struct src_tuple *ns;
		HASH_FIND(hh2, ce->c->addr_map, &addr, sizeof(addr), ns);
		sendbuf = malloc(sizeof(buf)+sizeof(addr)+4);
		char *realbuf = sendbuf;
		sendsize = 0;

		if (ce->c->t_type == TCP) {
			//Attach a length to the head
			*(uint32_t *)realbuf = htonl(ret);
			realbuf += 4;
			sendsize = 4;
		}

		if (!ns) {
			//Set the seq number to 0
			*(uint32_t *)realbuf = 0;
			realbuf += 4;
			//Send to ipv6 addr
			memcpy(realbuf, &addr.sin6_addr, sizeof(addr.sin6_addr));
			realbuf += sizeof(addr.sin6_addr);
			//Send port number in network order
			*(uint16_t *)(realbuf) = addr.sin6_port;
			realbuf += 2;

			struct src_tuple *st = talloc(1, struct src_tuple);
			memcpy(&st->addr, &addr, sizeof(addr));
			do {
				st->id = random();
				HASH_FIND_INT(ce->c->id_map, &st->id, ns);
			}while(ns);
			HASH_ADD_INT(ce->c->id_map, id, st);
			HASH_ADD_KEYPTR(hh2, ce->c->addr_map, &st->addr,
					sizeof(st->addr), st);
			*(uint32_t *)realbuf = st->id;
			realbuf += 4;

			memcpy(realbuf, buf, ret);
			sendsize += ret+10+sizeof(addr.sin6_addr);
		}
	}else
		sendbuf = recvbuf;

	ret = send(ce->c->t_fd, sendbuf, sendsize, 0);
	if (ret < 0 && errno == EMSGSIZE) {
		char ipv6_addr[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &addr.sin6_addr, ipv6_addr, sizeof(addr.sin6_addr));
		fprintf(stderr, "Received a large msg from %s, discard.", ipv6_addr);
	}
}

void transfer_cb(EV_P_ ev_io *w, int revent){
	fprintf(stderr, "remote port send data\n");
	struct conn_event *ce = (struct conn_event *)w;
	ssize_t ret;
	struct sockaddr_in6 addr;
	socklen_t addrlen;
	if (!ce->c->established) {
		//The first packet is for connection
		uint32_t seq;
		ret = recvfrom(ce->w.fd, &seq, 4, MSG_DONTWAIT|MSG_TRUNC, &addr,
			       &addrlen);
		if (ntohl(seq) != ce->c->seq) {
			//Seq doesn't match
			return;
		}
		connect(ce->w.fd, &addr, addrlen);
		return;
	}

	if (ce->c->out_type == UDP) {
		//Decode UDP packet.
		uint32_t len;
		if (ce->c->t_type == TCP) {
			//Read the length
			ret = recv(ce->w.fd, &len, sizeof(len), MSG_DONTWAIT);
			if (ret < 0)
				return;
		}else{
			char buf[2];
			ret = recv(ce->w.fd, buf, sizeof(buf), MSG_DONTWAIT|MSG_PEEK|MSG_TRUNC);
			len = ret;
		}
		//There will be a 4 bytes seq
		char *buf = malloc(len+4);
		ret = recv(ce->w.fd, buf, len+4, MSG_DONTWAIT);
		uint32_t seq = *(uint32_t *)buf;
		seq = ntohl(seq);

		struct src_tuple *st = NULL;
		HASH_FIND_INT(ce->c->id_map, &seq, st);
		if (!st) {
			free(buf);
			return;
		}
		ret = sendto(ce->c->out_fd, buf+4, len, 0, &st->addr, sizeof(st->addr));
		free(buf);
	} else {
		char *buf = malloc(4096);
		ret = recv(ce->c->t_fd, buf, 4096, MSG_DONTWAIT);
		if (ret == 0) {
			fprintf(stderr, "FIXME: Remote end closed, should close connection\n");
			ev_io_stop(EV_A_ w);
			return;
		}
		if (ret < 0)
			return;
		ret = send(ce->c->out_fd, buf, 4096, 0);
	}
}

void general_listen_cb(EV_P_ ev_io *w, int revent){
	struct sockaddr_in6 addr;
	struct conn_event *ce = (struct conn_event *)w;
	socklen_t addrlen = sizeof(addr);
	int ret = accept(w->fd, &addr, &addrlen);
	if (ret < 0)
		return;

	close(w->fd);
	ev_io_stop(EV_A_ w);
	ev_io_set(w, ret, EV_READ);
	ev_set_cb(w, ce->cb);
	ev_io_start(EV_A_ w);

	if (ce->user_cb)
		ce->user_cb(EV_A_ ce, revent);
}

int setup_data_sock(EV_P_ struct sockaddr_in6 *a, struct connection *c,
		    int type, void *cb, void *user_cb){
	struct conn_event *ev;
	int fd = setup_socket(a, type);
	ev = talloc(1, struct conn_event);
	ev->c = c;
	ev->cb = cb;
	ev->user_cb = user_cb;
	if (type == TCP)
		ev_io_init((ev_io *)ev, general_listen_cb, fd, EV_READ);
	else
		ev_io_init((ev_io *)ev, ev->cb, fd, EV_READ);
	ev_io_start(EV_A_ (ev_io *)ev);
	return fd;
}

void control_cb(EV_P_ ev_io *w, int revent){
	struct control_event *ce = (struct control_event *)w;
	struct control_packet cp, res;
	struct connection *c, *tmp;
	struct sockaddr_in6 addr, paddr;
	int fd = ce->w.fd;
	socklen_t paddrlen;

	int ret = recvfrom(fd, &cp, sizeof(struct control_packet),
			   MSG_DONTWAIT, &paddr, &paddrlen);
	if (ret < 0)
		return;

	if (ret == 0 && ce->type == TCP)
		goto destroy;

	int seq = ntohl(cp.seq);
	int value = ntohl(cp.value);
	res.seq = cp.seq;
	res.action = cp.action;

	switch(cp.action) {
		case OPEN_TCP:
		case OPEN_UDP:
			fprintf(stderr, "Received OPEN, port %u\n", ntohs(value));
			c = talloc(1, struct connection);
			c->out_type = cp.action;
			c->t_type = ce->type;
			c->ctrl_fd = ce->w.fd;

			memset(&addr, 0, sizeof(addr));
			addr.sin6_family = AF_INET6;
			addr.sin6_port = value;
			c->out_fd = setup_data_sock(EV_A_ &addr, c, c->out_type,
					world_cb, world_listen_cb);

			do {
				tmp = NULL;
				c->seq = random();
				HASH_FIND_INT(ce->conns, &c->seq, tmp);
			}while(tmp);
			HASH_ADD_INT(ce->conns, seq, c);

			res.value = htonl(c->seq);
			sendto(fd, &res, sizeof(res), 0, &paddr, paddrlen);
			break;
		case TRANSFER_PORT:
			fprintf(stderr, "Received TRANSFER_PORT\n");
			//The client is asking for a transfer port
			HASH_FIND_INT(ce->conns, &value, c);
			if (!c) {
				fprintf(stderr, "Connection seq doesn't exist, %d", value);
				break;
			}

			memcpy(&addr, &ce->addr, sizeof(addr));
			addr.sin6_port = random();
			c->t_fd = setup_data_sock(EV_A_ &addr, c, c->t_type,
					transfer_cb, transfer_listen_cb);
			fprintf(stderr, "Allocated transfer port %u\n", ntohs(addr.sin6_port));

			res.value = htonl(addr.sin6_port);
			sendto(fd, &res, sizeof(res), 0, &paddr, paddrlen);
			break;
		case CLOSE:
			fprintf(stderr, "Received CLOSE\n");
			HASH_FIND_INT(ce->conns, &value, c);
			if (!c) {
				res.value = (uint32_t)-1;
				write(fd, &res, sizeof(res));
			}else{
				HASH_DEL(ce->conns, c);
				if (c->t_type == TCP)
					shutdown(c->t_fd, SHUT_RDWR);
				else
					close(c->t_fd);

				if (c->out_type == TCP)
					shutdown(c->out_fd, SHUT_RDWR);
				else {
					close(c->out_fd);
					HASH_CLEAR(hh, c->id_map);
					HASH_CLEAR(hh2, c->addr_map);
				}
				res.value = 0;
				sendto(fd, &res, sizeof(res), 0, &paddr,
				       paddrlen);
				free(c);
			}
			break;
		case CLOSE_CTRL:
			if (ce->type != TCP) {
				//Ignore CLOSE_CTRL on udp control socket
				fprintf(stderr, "Received CLOSE_CTRL on UDP, bad\n");
				break;
			}
		destroy:
			fprintf(stderr, "Received CLOSE_CTRL on TCP\n");
			ev_io_stop(EV_A_ w);
			shutdown(fd, SHUT_RDWR);
			HASH_ITER(hh, ce->conns, c, tmp){
				//Close every connection
				HASH_DEL(ce->conns, c);
				if (c->t_type == TCP)
					shutdown(c->t_fd, SHUT_RDWR);
				if (c->out_type == TCP)
					shutdown(c->out_fd, SHUT_RDWR);
				free(c);
			}
			HASH_CLEAR(hh, ce->conns);
			free(ce);
			break;
		default:
			fprintf(stderr, "Unknow packet\n");
			break;
	}

}

void control_listen_cb(EV_P_ ev_io *w, int revent){
	struct control_event *e = (struct control_event *)w;
	struct sockaddr_in6 caddr;
	socklen_t addrlen = sizeof(caddr);
	int ret = accept(e->w.fd, &caddr, &addrlen);
	if (ret < 0)
		return;
	close(w->fd);

	ev_io_stop(EV_A_ w);
	ev_io_set(w, ret, EV_READ);
	ev_set_cb(w, control_cb);
	ev_io_start(EV_A_ w);

	int new_fd = setup_socket(&e->addr, TCP);
	struct control_event *ce = talloc(1, struct control_event);
	ev_io_init((ev_io *)ce, control_listen_cb, new_fd, EV_READ);
	ev_io_start(EV_A_ (ev_io *)ce);
}

int main(int argc, const char **argv){
	if (argc < 2) {
		fprintf(stderr, "Usage: %s address [tcp port [udp port]]\n", argv[0]);
		return 1;
	}
	int rfd = open("/dev/random", O_RDONLY);
	uint32_t seed;
	read(rfd, &seed, sizeof(seed));
	close(rfd);

	srandom(seed);
	struct ev_loop *loop = EV_DEFAULT;

	const char *colon = strchr(argv[1], ':');
	char *ip_addr;
	if (colon)
		ip_addr = strdup(argv[1]);
	else
		asprintf(&ip_addr, "::ffff:%s", argv[1]);

	struct control_event *ce = talloc(1, struct control_event);
	int ret = inet_pton(AF_INET6, ip_addr, &ce->addr.sin6_addr);
	ce->addr.sin6_family = AF_INET6;
	free(ip_addr);

	if (argc >= 3)
		ce->addr.sin6_port = htons(atoi(argv[2]));
	else
		ce->addr.sin6_port = htons(2200);

	int fd = setup_socket(&ce->addr, TCP);

	ev_io_init((ev_io *)ce, control_listen_cb, fd, EV_READ);
	ev_io_start(loop, (ev_io *)ce);

	fd = setup_socket(&ce->addr, UDP);
	struct control_event *ce2 = talloc(1, struct control_event);
	memcpy(&ce2->addr, &ce->addr, sizeof(ce->addr));
	if (argc >= 4)
		ce2->addr.sin6_port = htons(atoi(argv[3]));
	ev_io_init((ev_io *)ce2, control_cb, fd, EV_READ);
	ev_io_start(loop, (ev_io *)ce2);

	ev_run(loop, 0);
}
