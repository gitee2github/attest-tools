/*
 * Copyright (C) 2019 Huawei Technologies Duesseldorf GmbH
 *
 * Author: Roberto Sassu <roberto.sassu@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * File: attest_tls_client.c
 *      TLS client.
 */

/* Code taken from https://wiki.openssl.org/index.php/Simple_TLS_Server */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "attest_tls_common.h"

#define SERVER_PORT "4433"

static int create_socket(char *server_fqdn)
{
	struct addrinfo hints, *result = NULL, *rp;
	int rc, fd = -1;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	rc = getaddrinfo(server_fqdn, SERVER_PORT, &hints, &result);
	if (rc)
		return -1;

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype,
				rp->ai_protocol);
		if (fd == -1)
			continue;

		if (connect(fd, rp->ai_addr, rp->ai_addrlen) != -1)
			break;

		close(fd);
	}

	freeaddrinfo(result);

	if (!rp)
		return -1;

	return fd;
}

static int send_receive_attest_data(int server, char *attest_data_path,
				    size_t *server_attest_data_size,
				    unsigned char **server_attest_data)
{
	unsigned char *client_attest_data = NULL;
	size_t file_size = 0, data_size = 0;
	int rc;

	if (attest_data_path) {
		rc = attest_util_read_file(attest_data_path, &file_size,
					   &client_attest_data);
		if (!rc)
			data_size = file_size;
	}

	data_size = htonl(data_size);
	rc = attest_util_write_buf(server, (unsigned char *)&data_size,
				   sizeof(data_size));
	if (rc < 0)
		goto error;

	if (file_size) {
		rc = attest_util_write_buf(server, client_attest_data,
					   file_size);
		if (rc < 0)
			goto error;
	}

	rc = attest_util_read_buf(server, (unsigned char *)&data_size,
				  sizeof(data_size));
	if (rc < 0)
		goto error;

	data_size = ntohl(data_size);
	if (data_size) {
		*server_attest_data = malloc(data_size);
		if (!*server_attest_data) {
			rc = -ENOMEM;
			goto error;
		}

		rc = attest_util_read_buf(server, *server_attest_data,
					  data_size);
		if (rc < 0) {
			free(*server_attest_data);
			goto error;
		}

		*server_attest_data_size = data_size;

	}
error:
	if (client_attest_data)
		munmap(client_attest_data, file_size);

	return rc;
}

static struct option long_options[] = {
	{"key", 1, 0, 'k'},
	{"cert", 1, 0, 'c'},
	{"ca-certs", 1, 0, 'd'},
	{"server", 1, 0, 's'},
	{"attest-data", 1, 0, 'a'},
	{"engine", 0, 0, 'e'},
	{"pcr-list", 0, 0, 'p'},
	{"requirements", 1, 0, 'r'},
	{"verify-skae", 0, 0, 'S'},
	{"disable-custom-protocol", 0, 0, 'D'},
	{"verbose", 0, 0, 'V'},
	{"help", 0, 0, 'h'},
	{"version", 0, 0, 'v'},
	{0, 0, 0, 0}
};

static void usage(char *argv0)
{
	fprintf(stdout, "Usage: %s [options] <filename>\n\n"
		"Options:\n"
		"\t-k, --key                     client private key\n"
		"\t-c, --cert                    client certificate\n"
		"\t-d, --ca-certs                CA certificates\n"
		"\t-s, --server                  server FQDN\n"
		"\t-a, --attest-data             attestation data\n"
		"\t-e, --engine                  use tpm2 engine\n"
		"\t-p, --pcr-list                PCR list\n"
		"\t-r, --requirements            verifier requirements\n"
		"\t-S, --verify-skae             verify peer's SKAE\n"
		"\t-D, --disable-custom-protocol disable custom protocol\n"
		"\t-V, --verbose                 verbose mode\n"
		"\t-h, --help                    print this help message\n"
		"\t-v, --version                 print package version\n"
		"\n"
		"Report bugs to " PACKAGE_BUGREPORT "\n",
		argv0);
	exit(-1);
}

int main(int argc, char **argv)
{
	SSL_CTX *ctx;
	SSL *ssl;
	char request[256], reply[10];
	char *key_path = NULL, *cert_path = NULL, *ca_path = NULL;
	char *attest_data_path = NULL, *req_path = NULL;
	char *server_fqdn = NULL, *pcr_list_str = NULL, *logs;
	unsigned char *server_attest_data = NULL;
	size_t server_attest_data_size = 0, nbytes, total = 0;
	int server, option_index, c, custom_protocol = 1;
	int rc = -EINVAL, engine = 0, verify_skae = 0, verbose = 0;

	setvbuf(stdout, NULL, _IONBF, 1);

	while (1) {
		option_index = 0;
		c = getopt_long(argc, argv, "k:c:d:s:a:ep:r:SDVhv", long_options,
				&option_index);
		if (c == -1)
			break;

		switch (c) {
			case 'k':
				key_path = optarg;
				break;
			case 'c':
				cert_path = optarg;
				break;
			case 'd':
				ca_path = optarg;
				break;
			case 's':
				server_fqdn = optarg;
				break;
			case 'a':
				attest_data_path = optarg;
				break;
			case 'e':
				engine = 1;
				break;
			case 'p':
				pcr_list_str = optarg;
				break;
			case 'r':
				req_path = optarg;
				break;
			case 'S':
				verify_skae = 1;
				break;
			case 'V':
				verbose = 1;
				break;
			case 'D':
				custom_protocol = 0;
				break;
			case 'h':
				usage(argv[0]);
				break;
			case 'v':
				fprintf(stdout, "%s " VERSION "\n"
					"Copyright 2019 by Roberto Sassu\n"
					"License GPLv2: GNU GPL version 2\n"
					"Written by Roberto Sassu <roberto.sassu@huawei.com>\n",
					argv[0]);
				exit(0);
			default:
				printf("Unknown option '%c'\n", c);
				usage(argv[0]);
				break;
		}
	}

	if (!server_fqdn) {
		printf("Missing TLS server\n");
		return -EINVAL;
	}

	if (verify_skae && !req_path) {
		printf("Missing requirements\n");
		return -EINVAL;
	}

	init_openssl();

	ctx = create_context(CONTEXT_CLIENT);
	if (!ctx)
		goto cleanup;

	rc = configure_context(ctx, engine, verify_skae, key_path, cert_path,
			       ca_path);
	if (rc < 0)
		goto cleanup;

	server = create_socket(server_fqdn);
	if (server < 0) {
		perror("Unable to connect");
		goto free;
	}

	attest_ctx_data_init(NULL);
	attest_ctx_verifier_init(NULL);

	if (custom_protocol) {
		rc = send_receive_attest_data(server, attest_data_path,
					      &server_attest_data_size,
					      &server_attest_data);
		if (rc < 0)
			goto error;
	}

	if (verify_skae) {
		rc = configure_attest(server, server_attest_data_size,
				      server_attest_data, pcr_list_str,
				      req_path);
		if (rc < 0)
			goto error;
	}

	ssl = SSL_new(ctx);
	SSL_set_fd(ssl, server);

	rc = SSL_connect(ssl);

	if (verify_skae && verbose) {
		logs = attest_ctx_verifier_result_print_json(
					attest_ctx_verifier_get_global());
		printf("%s\n", logs);
		free(logs);
	}

	if (rc <= 0) {
		ERR_print_errors_fp(stderr);
		close(server);
		rc = -EIO;
		goto error_ssl;
	}

	if (!SSL_get_verify_result(ssl) == X509_V_OK) {
		ERR_print_errors_fp(stderr);
		printf("bad server cert\n");
		goto error_ssl;
	}

	printf("good server cert\n");
	if (custom_protocol) {
		SSL_read(ssl, reply, sizeof(reply) - 1);
	} else {
		snprintf(request, sizeof(request),
			 "GET / HTTP/1.1\r\n"
			 "Host: %s\r\n"
			 "Connection: close\r\n\r\n\n", server_fqdn);
		SSL_write(ssl, request, strlen(request));
		while (SSL_read_ex(ssl, reply, sizeof(reply) - 1, &nbytes) > 0) {
			total += nbytes;
			reply[nbytes] = '\0';
			printf("%s", reply);
		}

		printf("Server returned %ld bytes\n", total);
	}
error_ssl:
	SSL_shutdown(ssl);
	SSL_free(ssl);
error:
	close(server);
free:
	SSL_CTX_free(ctx);
cleanup:
	cleanup_openssl();
	free(server_attest_data);
	attest_ctx_data_cleanup(NULL);
	attest_ctx_verifier_cleanup(NULL);
	return rc;
}
