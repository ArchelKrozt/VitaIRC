#ifndef VITAIRC_CONN_H
#define VITAIRC_CONN_H

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>

typedef struct {
	int fd;
	int use_ssl;
	int tls_ready;
	int cert_ok;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt ca;
	char rbuf[8192];
	int rlen;
} Conn;

void conn_global_init(void);

/* Blocking connect (with TLS handshake if use_ssl). Returns 0 on success. */
int  conn_open(Conn *c, const char *host, int port, int use_ssl, char *err, int errlen);
int  conn_write(Conn *c, const char *data, int len);
/* Reads one CRLF-terminated line (without the terminator).
 * Returns line length (>=0), -2 if no complete line within timeout, -1 on error/close. */
int  conn_readline(Conn *c, char *out, int max, int timeout_ms);
void conn_close(Conn *c);

#endif
