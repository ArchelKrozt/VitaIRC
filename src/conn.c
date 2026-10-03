#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <psa/crypto.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>

#include "conn.h"
#include "config.h"
#include "i18n.h"

void conn_global_init(void)
{
	psa_crypto_init();
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	Conn *c = ctx;
	int r = send(c->fd, buf, len, 0);
	if (r < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		return MBEDTLS_ERR_NET_SEND_FAILED;
	}
	return r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	Conn *c = ctx;
	int r = recv(c->fd, buf, len, 0);
	if (r < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return MBEDTLS_ERR_SSL_WANT_READ;
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
	if (r == 0)
		return MBEDTLS_ERR_NET_CONN_RESET;
	return r;
}

static int tcp_connect(const char *host, int port, char *err, int errlen)
{
	struct addrinfo hints, *res = NULL;
	char portstr[8];
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(portstr, sizeof(portstr), "%d", port);
	if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
		snprintf(err, errlen, T("No se pudo resolver %s"), host);
		return -1;
	}
	int fd = -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		snprintf(err, errlen, T("No se pudo conectar a %s:%d"), host, port);
		return -1;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	return fd;
}

int conn_open(Conn *c, const char *host, int port, int use_ssl, char *err, int errlen)
{
	memset(c, 0, sizeof(*c));
	c->fd = tcp_connect(host, port, err, errlen);
	if (c->fd < 0)
		return -1;
	c->use_ssl = use_ssl;
	if (!use_ssl)
		return 0;

	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->conf);
	mbedtls_entropy_init(&c->entropy);
	mbedtls_ctr_drbg_init(&c->drbg);
	mbedtls_x509_crt_init(&c->ca);
	c->tls_ready = 1;

	int r;
	const char *pers = "vitairc";
	if ((r = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
	                               (const unsigned char *)pers, strlen(pers))) != 0)
		goto fail;
	if ((r = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
	                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
		goto fail;

	/* Like the original daemon (rejectUnauthorized:false) we never refuse the
	 * connection over the certificate, but we verify it and report the result. */
	int have_ca = mbedtls_x509_crt_parse_file(&c->ca, CA_PATH) >= 0;
	mbedtls_ssl_conf_authmode(&c->conf, have_ca ? MBEDTLS_SSL_VERIFY_OPTIONAL : MBEDTLS_SSL_VERIFY_NONE);
	if (have_ca)
		mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

	if ((r = mbedtls_ssl_setup(&c->ssl, &c->conf)) != 0)
		goto fail;
	if ((r = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0)
		goto fail;
	mbedtls_ssl_set_bio(&c->ssl, c, bio_send, bio_recv, NULL);

	while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
			goto fail;
	}
	c->cert_ok = have_ca && mbedtls_ssl_get_verify_result(&c->ssl) == 0;
	return 0;

fail: {
		char ebuf[96];
		mbedtls_strerror(r, ebuf, sizeof(ebuf));
		snprintf(err, errlen, T("Error TLS: %s (-0x%04x)"), ebuf, (unsigned)-r);
		conn_close(c);
		return -1;
	}
}

int conn_write(Conn *c, const char *data, int len)
{
	int off = 0;
	while (off < len) {
		int r;
		if (c->use_ssl) {
			r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)data + off, len - off);
			if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
		} else {
			r = send(c->fd, data + off, len - off, 0);
		}
		if (r <= 0)
			return -1;
		off += r;
	}
	return 0;
}

static int extract_line(Conn *c, char *out, int max)
{
	for (int i = 0; i < c->rlen; i++) {
		if (c->rbuf[i] != '\n')
			continue;
		int n = i;
		if (n > 0 && c->rbuf[n - 1] == '\r')
			n--;
		int copy = n < max - 1 ? n : max - 1;
		memcpy(out, c->rbuf, copy);
		out[copy] = 0;
		memmove(c->rbuf, c->rbuf + i + 1, c->rlen - i - 1);
		c->rlen -= i + 1;
		return copy;
	}
	if (c->rlen == sizeof(c->rbuf)) {     /* overlong line: drop it */
		c->rlen = 0;
	}
	return -2;
}

int conn_readline(Conn *c, char *out, int max, int timeout_ms)
{
	int r = extract_line(c, out, max);
	if (r != -2)
		return r;

	if (!(c->use_ssl && mbedtls_ssl_get_bytes_avail(&c->ssl) > 0)) {
		struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
		int p = poll(&pfd, 1, timeout_ms);
		if (p == 0)
			return -2;
		if (p < 0)
			return errno == EINTR ? -2 : -1;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			if (!(pfd.revents & POLLIN))
				return -1;
		}
	}

	int space = sizeof(c->rbuf) - c->rlen;
	if (c->use_ssl) {
		r = mbedtls_ssl_read(&c->ssl, (unsigned char *)c->rbuf + c->rlen, space);
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
		    || r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
#endif
		   )
			return -2;
	} else {
		r = recv(c->fd, c->rbuf + c->rlen, space, 0);
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return -2;
	}
	if (r <= 0)
		return -1;
	c->rlen += r;
	return extract_line(c, out, max);
}

void conn_close(Conn *c)
{
	if (c->tls_ready) {
		if (c->fd >= 0)
			mbedtls_ssl_close_notify(&c->ssl);
		mbedtls_ssl_free(&c->ssl);
		mbedtls_ssl_config_free(&c->conf);
		mbedtls_ctr_drbg_free(&c->drbg);
		mbedtls_entropy_free(&c->entropy);
		mbedtls_x509_crt_free(&c->ca);
		c->tls_ready = 0;
	}
	if (c->fd >= 0) {
		shutdown(c->fd, SHUT_RDWR);
		close(c->fd);
	}
	c->fd = -1;
	c->rlen = 0;
}
