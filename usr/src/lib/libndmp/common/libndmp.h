/*
 * BSD 3 Clause License
 *
 * Copyright (c) 2007, The Storage Networking Industry Association.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 	- Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 *
 * 	- Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in
 *	  the documentation and/or other materials provided with the
 *	  distribution.
 *
 *	- Neither the name of The Storage Networking Industry Association (SNIA)
 *	  nor the names of its contributors may be used to endorse or promote
 *	  products derived from this software without specific prior written
 *	  permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 2012 by Delphix. All rights reserved.
 */

#ifndef _LIBNDMP_H
#define	_LIBNDMP_H

#include <sys/types.h>
#include <sys/socket.h>

#include <ndmp/ndmp.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * The NDMP library exposes an NDMP server and client implementation.
 *
 * The NDMP server handles all aspects of communication as well as tape and
 * SCSI device management.  It doesn't have any knowledge about the data
 * semantics, including namespace and backup format.  It is up to the consumer
 * to provide callbacks for data operations.  The server is controlled via
 * start() and stop() methods, which manage the asynchronously running server.
 *
 * The NDMP client similarly handles the communication layer, exposing just
 * the bare primitives necessary to implement basic tools.  Unlike the server,
 * it is not designed to be a full-featured DMA, but instead has the minimal
 * set of functionality to facilitate testing (raw command processing and 3-way
 * restore).
 */

struct ndmp_client;
struct ndmp_msg;
struct ndmp_session;
struct ndmp_server;

/*
 * Configuration settings shared across server and client.  This includes the
 * ability to log messages using syslog LOG_* levels, as well as the ability to
 * fetch properties.  For more information on what property names are
 * available, see ndmp_prop.c.
 */
typedef struct ndmp_common_conf {
	void (*nc_log)(struct ndmp_session *, int, const char *);
	const char *(*nc_get_prop)(const char *);
} ndmp_common_conf_t;

/* Server configuration  */
typedef struct ndmp_server_conf {
	/* common to both client and server */
	ndmp_common_conf_t ns_common;

	/* fields provided by the plugin */
	const char **ns_types;			/* backup types */
	const char *ns_vendor;			/* vendor name */
	const char *ns_product;			/* product name */
	const char *ns_revision;		/* product revision */

	/* functions provided by the plugin */
	int (*ns_session_register)(struct ndmp_session *);
	void (*ns_session_unregister)(struct ndmp_session *);
	const ndmp_pval *(*ns_get_backup_env)(const char *, int);
	ulong_t (*ns_get_backup_attrs)(const char *);
	int (*ns_auth_text)(struct ndmp_session *, const char *, const char *);
	int (*ns_auth_md5)(struct ndmp_session *, const char *, const char *,
	    const unsigned char *);
	int (*ns_start_backup)(struct ndmp_session *, const char *);
	int (*ns_start_recover)(struct ndmp_session *, const char *);
	void (*ns_abort)(struct ndmp_session *, boolean_t);
	int (*ns_list_fs)(struct ndmp_session *);
	void (*ns_tape_opened)(struct ndmp_session *, int);
} ndmp_server_conf_t;

/* Main server control */
extern struct ndmp_server *ndmp_server_create(ndmp_server_conf_t *);
extern int ndmp_server_start(struct ndmp_server *);
extern void ndmp_server_stop(struct ndmp_server *);
extern void ndmp_server_destroy(struct ndmp_server *);

/* Server callback support functions */
extern void ndmp_server_done(struct ndmp_session *, int);
extern void ndmp_server_log(struct ndmp_session *, int, const char *);
extern int ndmp_server_read(struct ndmp_session *, void *, ssize_t);
extern int ndmp_server_write(struct ndmp_session *, const void *, ulong_t);
extern int ndmp_server_seek(struct ndmp_session *, u_longlong_t, u_longlong_t);
extern int ndmp_server_file_recovered(struct ndmp_session *, const char *, int);
extern void *ndmp_server_get_name(struct ndmp_session *, int);
extern const char *ndmp_server_env_name(struct ndmp_session *, int);
extern int ndmp_server_env_set(struct ndmp_session *, const char *,
    const char *);
extern const char *ndmp_server_env_value(struct ndmp_session *, int);
extern void ndmp_server_md5_digest(unsigned char *, const char *,
    unsigned char *);
extern const char *ndmp_server_remote_addr(struct ndmp_session *);
extern int ndmp_server_add_fs(struct ndmp_session *, ndmp_fs_info_v3 *);
extern u_longlong_t ndmp_server_bytes_processed(struct ndmp_session *);
extern int ndmp_server_data_error(struct ndmp_session *);

/* Client configuration */
typedef struct ndmp_client_conf {
	ndmp_common_conf_t nc_common;
	void (*nc_log_remote)(struct ndmp_session *, int, const char *);
} ndmp_client_conf_t;

/* Main client control */
extern struct ndmp_client *ndmp_client_create(ndmp_client_conf_t *);
extern void ndmp_client_destroy(struct ndmp_client *);
extern struct ndmp_session *ndmp_client_connect(struct ndmp_client *,
    const char *, int);
extern void ndmp_client_disconnect(struct ndmp_session *);

/* Raw message handling */
extern struct ndmp_msg *ndmp_client_send(struct ndmp_session *,
    ndmp_message, void *);
extern void *ndmp_client_msg_body(struct ndmp_msg *);
extern int ndmp_client_msg_error(struct ndmp_msg *);
extern void ndmp_client_msg_free(struct ndmp_session *, struct ndmp_msg *);

/* Session establishment */
extern int ndmp_client_open(struct ndmp_session *);
extern int ndmp_client_close(struct ndmp_session *);
extern int ndmp_client_authenticate(struct ndmp_session *, const char *,
    const char *, ndmp_auth_type);

/* Configuration queries */
typedef void ndmp_butypes_t;

extern struct ndmp_msg *ndmp_client_butypes_get(struct ndmp_session *);
extern ndmp_butype_info *ndmp_client_butypes_info(struct ndmp_session *,
    struct ndmp_msg *, int);
extern struct ndmp_msg *ndmp_client_fs_list(struct ndmp_session *);
extern ndmp_fs_info_v3 *ndmp_client_fs_info(struct ndmp_session *,
    struct ndmp_msg *, int);


/* Data management */
typedef void ndmp_addr_t;

extern ndmp_addr_t *ndmp_client_data_listen(struct ndmp_session *);
extern int ndmp_client_data_connect(struct ndmp_session *, ndmp_addr_t *);
extern int ndmp_client_data_stop(struct ndmp_session *);
extern void ndmp_client_addr_free(struct ndmp_session *, ndmp_addr_t *);
extern int ndmp_client_start_backup(struct ndmp_session *, const char *,
    int, ndmp_pval *);
extern int ndmp_client_start_recover(struct ndmp_session *, const char *,
    int, ndmp_pval *, int, ndmp_name_v3 *);

/* Mover control */
extern int ndmp_client_mover_abort(struct ndmp_session *);
extern int ndmp_client_mover_close(struct ndmp_session *);
extern int ndmp_client_mover_stop(struct ndmp_session *);
extern int ndmp_client_mover_set_record_size(struct ndmp_session *,
    size_t);
extern int ndmp_client_mover_set_window(struct ndmp_session *,
    u_longlong_t, u_longlong_t);
extern ndmp_addr_t *ndmp_client_mover_listen(struct ndmp_session *,
    ndmp_mover_mode mode);
extern int ndmp_client_mover_read(struct ndmp_session *,
    u_longlong_t, u_longlong_t);
extern int ndmp_client_mover_connect(struct ndmp_session *, ndmp_addr_t *,
    ndmp_mover_mode mode);

/* Tape control */
extern int ndmp_client_tape_close(struct ndmp_session *);
extern int ndmp_client_tape_open(struct ndmp_session *, const char *,
    ndmp_tape_open_mode);
extern int ndmp_client_tape_read(struct ndmp_session *, char *, size_t);
extern int ndmp_client_tape_write(struct ndmp_session *, const char *, size_t);

/* Environment management */
extern int ndmp_client_env_refresh(struct ndmp_session *);
extern const char *ndmp_client_env_name(struct ndmp_session *, int);
extern const char *ndmp_client_env_value(struct ndmp_session *, int);

/* Notification handling */
typedef struct ndmp_notification {
	ndmp_message nn_message;
	char *nn_text;
	int nn_reason;
	u_longlong_t nn_offset;
	u_longlong_t nn_length;
	struct ndmp_notification *nn_prev;
	struct ndmp_notification *nn_next;
} ndmp_notification_t;

extern ndmp_notification_t *ndmp_client_wait_notify(struct ndmp_session *,
    uint_t, int *);
extern void ndmp_client_notify_free(struct ndmp_session *,
    ndmp_notification_t *);

/*
 * Common to both client and server implementations.
 */
extern int ndmp_session_error(struct ndmp_session *);

#ifdef	__cplusplus
}
#endif

#endif /* _LIBNDMP_H */
