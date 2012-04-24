/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 */

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
/* Copyright (c) 2007, The Storage Networking Industry Association. */
/* Copyright (c) 1996, 1997 PDC, Network Appliance. All Rights Reserved */
/* Copyright (c) 2012 by Delphix. All rights reserved. */

#include "ndmp_impl.h"

/*
 * Server side support functions.  This includes both functions for consumers to
 * manipulate server state, as well as callbacks to to be invoked from async
 * backup/restore context.
 */

#define	NDMP_LISTEN_BACKLOG	5

/*
 * Create a new server instance.
 */
ndmp_server_t *
ndmp_server_create(ndmp_server_conf_t *conf)
{
	ndmp_server_t *server;
	ndmp_session_t *session;

	if ((server = calloc(sizeof (ndmp_server_t), 1)) == NULL) {
		conf->ns_common.nc_log(NULL, LOG_ERR, "out of memory");
		return (NULL);
	}

	server->ns_conf = conf;
	session = &server->ns_global_session;
	session->ns_server = server;
	session->ns_global = B_TRUE;
	session->ns_conf = &conf->ns_common;

	if (ndmp_load_prop(session) != 0) {
		free(server);
		return (NULL);
	}

	ndmp_debug(session, "NDMP server initialized");
	ndmp_debug(session, "vendor = \"%s\"", conf->ns_vendor);
	ndmp_debug(session, "product = \"%s\"", conf->ns_product);
	ndmp_debug(session, "revision = \"%s\"", conf->ns_product);

	return (server);
}

/*
 * Destroy a server instance.  This will stop the server if the consumer has
 * not already done so.
 */
void
ndmp_server_destroy(ndmp_server_t *server)
{
	ndmp_server_stop(server);
	ndmp_device_fini(server);
	free(server);
}

/*
 * Start the server.
 */
int
ndmp_server_start(ndmp_server_t *server)
{
	ndmp_session_t *session = &server->ns_global_session;
	int on;
	struct sockaddr_in sin;
	int port;

	/*
	 * Initialize the tape library manager.  The set of available tape
	 * devices and the method by which we interact with them is global to
	 * the server, not specific to any paritcular session.
	 */
	if (ndmp_device_init(server) == -1) {
		ndmp_log(session, LOG_ERR, "failed to initialize device list");
		return (-1);
	}

	server->ns_shutdown = B_FALSE;

	/*
	 * We setup the listening socket in synchronous context both because it
	 * is the most likely thing to fail, and because it guarantees that
	 * calling ndmp_start() immediately followed by ndmp_stop() will
	 * succeed.
	 */
	port = ndmp_get_prop_int(session, NDMP_TCP_PORT);

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = INADDR_ANY;
	sin.sin_port = htons(port);

	if ((server->ns_listen_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		ndmp_log(session, LOG_ERR,
		    "failed to create socket: %s", strerror(errno));
		return (-1);
	}

	on = 1;
	(void) setsockopt(server->ns_listen_socket, SOL_SOCKET, SO_REUSEADDR,
	    (char *)&on, sizeof (on));

	if (bind(server->ns_listen_socket, (struct sockaddr *)&sin,
	    sizeof (sin)) < 0) {
		ndmp_log(session, LOG_ERR,
		    "failed to bind socket: %s", strerror(errno));
		return (-1);
	}

	if (listen(server->ns_listen_socket, NDMP_LISTEN_BACKLOG) < 0) {
		ndmp_log(session, LOG_ERR,
		    "failed to liston on socket: %s", strerror(errno));
		return (-1);
	}

	if (pthread_create(&server->ns_listen_thread, NULL, ndmp_server_run,
	    server) != 0) {
		ndmp_log(session, LOG_ERR,
		    "failed to create main thread");
		return (-1);
	}

	ndmp_log(session, LOG_INFO, "NDMP server version %d running on port %d",
	    ndmp_get_prop_int(session, NDMP_MAX_VERSION), (int)port);

	return (0);
}

/*
 * Stop the server instance.  Invoking this function multiple times will have
 * no effect.
 */
void
ndmp_server_stop(ndmp_server_t *server)
{
	if (!server->ns_shutdown) {
		server->ns_shutdown = B_TRUE;

		/* Close the socket and wait for the accept thread to exit */
		(void) close(server->ns_listen_socket);
		(void) pthread_join(server->ns_listen_thread, NULL);

		/* Now close any active session */
		ndmp_session_list_teardown(&server->ns_session_list);
	}
}

/*
 * Get the restore name entry at the given index.  If the index exceeds the
 * current number of restore name requests, NULL is returned.
 */
void *
ndmp_server_get_name(ndmp_session_t *session, int name_index)
{
	if (name_index >= session->ns_data.dd_nlist_len)
		return (NULL);

	return (&session->ns_data.dd_nlist_v3[name_index]);
}

/*
 * Return the environment variable name at the given index.  If the index
 * exceeds the number of available environment entries, NULL is returned.
 */
const char *
ndmp_server_env_name(ndmp_session_t *session, int idx)
{
	if (idx >= session->ns_data.dd_env_len)
		return (NULL);

	return (session->ns_data.dd_env[idx].name);
}

/*
 * Set the given environment variable in the data environment.  This can be
 * used during backup to populate the environment that is retrieved when the
 * backup is complete.
 */
int
ndmp_server_env_set(ndmp_session_t *session, const char *name,
    const char *value)
{
	int i;
	char *namestr, *valuestr;
	ndmp_session_data_desc_t *dd = &session->ns_data;
	ndmp_pval *env;

	/*
	 * Technically, a GET ENV request could come in during a data backup
	 * operation that might be modifying the environment, so we have to
	 * lock the environment during this operation.
	 */
	(void) mutex_lock(&dd->dd_env_lock);

	/* Check to see if we should overwrite the value first */
	for (i = 0; i < dd->dd_env_len; i++) {
		if (strcmp(name, dd->dd_env[i].name) == 0) {
			valuestr = ndmp_strdup(session, value);
			if (valuestr == NULL) {
				(void) mutex_unlock(&dd->dd_env_lock);
				return (-1);
			}

			free(dd->dd_env[i].value);
			dd->dd_env[i].value = valuestr;
			(void) mutex_unlock(&dd->dd_env_lock);
			return (0);
		}
	}

	/* No such value, allocate a new one */
	namestr = ndmp_strdup(session, name);
	valuestr = ndmp_strdup(session, value);
	env = ndmp_malloc(session, sizeof (ndmp_pval) *
	    (dd->dd_env_len + 1));

	if (namestr == NULL || valuestr == NULL || env == NULL) {
		free(env);
		free(valuestr);
		free(namestr);
		(void) mutex_unlock(&dd->dd_env_lock);
		return (-1);
	}

	bcopy(dd->dd_env, env, sizeof (ndmp_pval) * dd->dd_env_len);
	env[dd->dd_env_len].name = namestr;
	env[dd->dd_env_len].value = valuestr;

	free(dd->dd_env);
	dd->dd_env = env;
	dd->dd_env_len++;

	(void) mutex_unlock(&dd->dd_env_lock);

	return (0);
}

/*
 * Return the environment variable variable at the given index.  If the index
 * exceeds the number of available environment entries, NULL is returned.
 */
const char *
ndmp_server_env_value(ndmp_session_t *session, int idx)
{
	if (idx >= session->ns_data.dd_env_len)
		return (NULL);

	return (session->ns_data.dd_env[idx].value);
}

/*
 * Indicate that the current data operation (backup or recover) is done for the
 * given reason (success or failure).
 */
void
ndmp_server_done(ndmp_session_t *session, int reason)
{
	(void) mutex_lock(&session->ns_lock);
	session->ns_running = B_FALSE;
	ndmp_data_error(session, reason);
	(void) mutex_unlock(&session->ns_lock);
}

/*
 * Log a request to the remote client.
 */
static void
ndmp_log_v3(ndmp_session_t *session, ndmp_log_type type, ulong_t msg_id,
    const char *buf)
{
	ndmp_log_message_request_v3 request;

	request.entry = (char *)buf;
	request.log_type = type;
	request.message_id = msg_id;

	(void) ndmp_send_request(session, NDMP_LOG_MESSAGE,
	    &request, NULL);
}

static void
ndmp_log_v4(ndmp_session_t *session, ndmp_log_type type, ulong_t msg_id,
    const char *buf)
{
	ndmp_log_message_request_v4 request;

	request.entry = (char *)buf;
	request.log_type = type;
	request.message_id = msg_id;
	request.associated_message_valid = NDMP_NO_ASSOCIATED_MESSAGE;
	request.associated_message_sequence = 0;

	(void) ndmp_send_request(session, NDMP_LOG_MESSAGE,
	    &request, NULL);
}

static void
ndmp_log_buf(ndmp_session_t *session, int level, char *buf)
{
	ndmp_log_type type;
	ulong_t msgid;

	if (session == NULL)
		return;

	switch (level) {
	case LOG_EMERG:
	case LOG_ALERT:
	case LOG_CRIT:
	case LOG_ERR:
		type = NDMP_LOG_ERROR;
		break;

	case LOG_WARNING:
		type = NDMP_LOG_WARNING;
		break;

	case LOG_NOTICE:
	case LOG_INFO:
		type = NDMP_LOG_NORMAL;
		break;

	default:
		assert(level == LOG_DEBUG);
		type = NDMP_LOG_DEBUG;
	}

	msgid = atomic_inc_32_nv(&session->ns_logid);

	switch (session->ns_version) {
	case NDMPV3:
		ndmp_log_v3(session, type, msgid, buf);
		break;

	case NDMPV4:
		ndmp_log_v4(session, type, msgid, buf);
		break;
	}
}

void
ndmp_server_log(ndmp_session_t *session, int level, const char *msg)
{
	char *buf;
	size_t len;

	/* Tack on an extra newline since the API doesn't do it for us */
	len = strlen(msg);
	buf = alloca(len + 2);
	(void) strcpy(buf, msg);
	buf[len] = '\n';
	buf[len + 1] = '\0';

	ndmp_log_buf(session, level, buf);
}

int
ndmp_server_read(ndmp_session_t *session, void *buf, ssize_t length)
{
	int ret;

	/*
	 * Clear data error state.
	 */
	session->ns_data.dd_errno = 0;

	if (session->ns_data.dd_data_addr.addr_type == NDMP_ADDR_LOCAL)
		ret = ndmp_local_read_v3(session, buf, length);
	else
		ret = ndmp_remote_read_v3(session, buf, length);

	if (ret > 0)
		session->ns_data.dd_bytes_processed += ret;

	return (ret);
}

int
ndmp_server_seek(ndmp_session_t *session, u_longlong_t offset,
    u_longlong_t length)
{
	int err;
	ndmp_notify_data_read_request request;

	session->ns_data.dd_read_offset = offset;
	session->ns_data.dd_read_length = length;

	/*
	 * Send a notify_data_read request if the mover is remote.
	 */
	if (session->ns_data.dd_data_addr.addr_type != NDMP_ADDR_LOCAL) {
		session->ns_data.dd_discard_length =
		    session->ns_data.dd_bytes_left_to_read;
		session->ns_data.dd_bytes_left_to_read = length;
		session->ns_data.dd_position = offset;

		request.offset = long_long_to_quad(offset);
		request.length = long_long_to_quad(length);

		if (ndmp_send_request(session,
		    NDMP_NOTIFY_DATA_READ, &request, NULL) < 0) {
			return (-1);
		}

		return (0);
	}

	/* Attempt the local seek */
	err = ndmp_mover_seek(session, offset, length);
	if (err < 0) {
		ndmp_mover_error(session, NDMP_MOVER_HALT_INTERNAL_ERROR);
		return (-1);
	}

	if (err == 0)
		return (0);

	/*
	 * NDMP client intervention is required to perform the seek.
	 * Wait for the client to either do the seek and send a continue
	 * request or send an abort request.
	 */
	err = ndmp_mover_wait(session);

	/*
	 * If we needed a client intervention, then we should be able to
	 * detect this in DAR.
	 */
	if (err == 0)
		err = 1;

	return (err);
}

static int
ndmp_file_recovered_v3(ndmp_session_t *session, char *name, int error)
{
	ndmp_log_file_request_v3 request;

	request.name  = name;

	switch (error) {
	case 0:
		request.error = NDMP_NO_ERR;
		break;
	case ENOENT:
		request.error = NDMP_FILE_NOT_FOUND_ERR;
		break;
	default:
		request.error = NDMP_PERMISSION_ERR;
	}

	return (ndmp_send_request(session, NDMP_LOG_FILE,
	    &request, NULL) < 0);
}

static int
ndmp_file_recovered_v4(ndmp_session_t *session, char *name, int error)
{
	ndmp_log_file_request_v4 request;

	request.name  = name;

	switch (error) {
	case 0:
		request.recovery_status = NDMP_RECOVERY_SUCCESSFUL;
		break;
	case EPERM:
		request.recovery_status = NDMP_RECOVERY_FAILED_PERMISSION;
		break;
	case ENOENT:
		request.recovery_status = NDMP_RECOVERY_FAILED_NOT_FOUND;
		break;
	case ENOTDIR:
		request.recovery_status = NDMP_RECOVERY_FAILED_NO_DIRECTORY;
		break;
	case ENOMEM:
		request.recovery_status = NDMP_RECOVERY_FAILED_OUT_OF_MEMORY;
		break;
	case EIO:
		request.recovery_status = NDMP_RECOVERY_FAILED_IO_ERROR;
		break;
	case EEXIST:
		request.recovery_status = NDMP_RECOVERY_FAILED_FILE_PATH_EXISTS;
		break;
	default:
		request.recovery_status = NDMP_RECOVERY_FAILED_UNDEFINED_ERROR;
		break;
	}

	if (ndmp_send_request(session, NDMP_LOG_FILE,
	    &request, NULL) < 0) {
		return (-1);
	}

	return (0);
}

int
ndmp_server_file_recovered(ndmp_session_t *session, const char *name, int error)
{
	if (session->ns_version == NDMPV3) {
		return (ndmp_file_recovered_v3(session, (char *)name,
		    error));
	} else {
		assert(session->ns_version == NDMPV4);
		return (ndmp_file_recovered_v4(session, (char *)name,
		    error));
	}
}

int
ndmp_server_write(ndmp_session_t *session, const void *data, ulong_t length)
{
	int ret;

	/*
	 * Clear data error state.
	 */
	session->ns_data.dd_errno = 0;

	/*
	 * Write the data to the tape if the mover is local, otherwise, write
	 * the data to the data connection.
	 */
	if (session->ns_data.dd_data_addr.addr_type == NDMP_ADDR_LOCAL)
		ret = ndmp_local_write_v3(session, (void *)data, length);
	else
		ret = ndmp_remote_write(session, (void *)data, length);

	if (ret == 0)
		session->ns_data.dd_bytes_processed += length;

	return (ret);
}

/*
 * Compute the MD5 digest for a given password and challenge using the standard
 * method.
 */
void
ndmp_server_md5_digest(unsigned char *buf, const char *password,
    unsigned char *challenge)
{
	ndmp_create_md5_digest(buf, password, challenge);
}

/*
 * Return a string indicating the remote client to which we are connected.
 */
const char *
ndmp_server_remote_addr(ndmp_session_t *session)
{
	return (session->ns_remoteaddr);
}

int
ndmp_server_add_fs(ndmp_session_t *session, ndmp_fs_info_v3 *fsinfo)
{
	ndmp_fs_info_v3 *fsip;
	ndmp_pval *envp;
	int i;

	if (session->ns_fsinfo_count == session->ns_fsinfo_alloc) {
		if ((fsip = ndmp_realloc(session, session->ns_fsinfo,
		    session->ns_fsinfo_alloc * 2 *
		    sizeof (ndmp_fs_info_v3))) == NULL) {
			return (-1);
		}

		session->ns_fsinfo_alloc *= 2;
		session->ns_fsinfo = fsip;
	}

	fsip = &session->ns_fsinfo[session->ns_fsinfo_count];

	fsip->invalid = fsinfo->invalid;
	fsip->total_size = fsinfo->total_size;
	fsip->used_size = fsinfo->used_size;
	fsip->avail_size = fsinfo->avail_size;
	fsip->total_inodes = fsinfo->total_inodes;
	fsip->used_inodes = fsinfo->used_inodes;

	fsip->fs_logical_device = ndmp_strdup(session,
	    fsinfo->fs_logical_device);
	fsip->fs_physical_device = ndmp_strdup(session,
	    fsinfo->fs_physical_device);
	fsip->fs_type = ndmp_strdup(session, fsinfo->fs_type);
	fsip->fs_status = ndmp_strdup(session, fsinfo->fs_status);

	if (fsip->fs_logical_device == NULL ||
	    fsip->fs_type == NULL || fsip->fs_status == NULL) {
		goto nomem;
	}

	if (fsinfo->fs_env.fs_env_len != 0 &&
	    (fsip->fs_env.fs_env_val = ndmp_malloc(session,
	    fsinfo->fs_env.fs_env_len * sizeof (ndmp_pval))) == NULL) {
		goto nomem;
	}
	fsip->fs_env.fs_env_len = fsinfo->fs_env.fs_env_len;

	for (i = 0; i < fsip->fs_env.fs_env_len; i++) {
		envp = &fsip->fs_env.fs_env_val[i];
		if ((envp->name = ndmp_strdup(session,
		    fsinfo->fs_env.fs_env_val[i].name)) == NULL ||
		    (envp->value = ndmp_strdup(session,
		    fsinfo->fs_env.fs_env_val[i].value)) == NULL) {
			goto nomem;
		}
	}

	session->ns_fsinfo_count++;

	return (0);

nomem:
	for (i = 0; i < fsip->fs_env.fs_env_len; i++) {
		free(fsip->fs_env.fs_env_val[i].name);
		free(fsip->fs_env.fs_env_val[i].value);
	}
	free(fsip->fs_status);
	free(fsip->fs_type);
	free(fsip->fs_logical_device);

	return (-1);
}

u_longlong_t
ndmp_server_bytes_processed(ndmp_session_t *session)
{
	return (session->ns_data.dd_bytes_processed);
}

int
ndmp_server_data_error(ndmp_session_t *session)
{
	return (session->ns_data.dd_errno);
}
