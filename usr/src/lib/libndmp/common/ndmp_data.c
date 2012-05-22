/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
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

static ndmp_error
data_connect_sock_v3(ndmp_session_t *session, ulong_t addr, ushort_t port)
{
	int sock;

	sock = ndmp_connect_sock_v3(session, addr, port);
	if (sock < 0)
		return (NDMP_CONNECT_ERR);

	session->ns_data.dd_sock = sock;
	session->ns_data.dd_data_addr.addr_type = NDMP_ADDR_TCP;
	session->ns_data.dd_data_addr.tcp_ip_v3 = ntohl(addr);
	session->ns_data.dd_data_addr.tcp_port_v3 = port;

	ndmp_debug(session, "data socket: %d\n", session->ns_data.dd_sock);

	return (NDMP_NO_ERR);
}

/*
 * Accept a data connection from a remote mover.  Called by ndmp_select when a
 * session is pending on the data listen socket.
 */
/*ARGSUSED*/
static void
data_accept_connection_v3(ndmp_session_t *session, int fd, ulong_t mode)
{
	int from_len;
	struct sockaddr_in from;
	int flag = 1;

	from_len = sizeof (from);
	session->ns_data.dd_sock = accept(fd, (struct sockaddr *)&from,
	    &from_len);

	ndmp_debug(session, "accepting connection on socket %d, "
	    "port %d, addr %s", session->ns_data.dd_sock,
	    ntohs(from.sin_port), inet_ntoa(IN_ADDR(from.sin_addr.s_addr)));

	ndmp_remove_file_handler(session, fd);
	(void) close(session->ns_data.dd_listen_sock);
	session->ns_data.dd_listen_sock = -1;

	if (session->ns_data.dd_sock < 0) {
		ndmp_log(session, LOG_ERR,
		    "failed to accept socket session: %s",
		    strerror(errno));
		ndmp_data_error(session, NDMP_DATA_HALT_CONNECT_ERROR);
		return;
	}

	/*
	 * Save the peer address.
	 */
	session->ns_data.dd_data_addr.tcp_ip_v3 = from.sin_addr.s_addr;
	session->ns_data.dd_data_addr.tcp_port_v3 = from.sin_port;

	/*
	 * Set the parameter of the new socket.
	 */
	(void) setsockopt(session->ns_data.dd_sock, SOL_SOCKET, SO_KEEPALIVE,
	    &flag, sizeof (flag));
	ndmp_set_socket_nodelay(session->ns_data.dd_sock);
	if (ndmp_sbs > 0)
		ndmp_set_socket_snd_buf(session, session->ns_data.dd_sock,
		    ndmp_sbs * KILOBYTE);
	if (ndmp_rbs > 0)
		ndmp_set_socket_rcv_buf(session, session->ns_data.dd_sock,
		    ndmp_rbs * KILOBYTE);

	session->ns_data.dd_state = NDMP_DATA_STATE_CONNECTED;
}


/*
 * Creates the data sockets for listening for a remote mover/data incoming
 * sessions.
 */
static int
create_listen_socket_v3(ndmp_session_t *session, ulong_t *addr, ushort_t *port)
{
	session->ns_data.dd_listen_sock = ndmp_create_socket(session,
	    addr, port);
	if (session->ns_data.dd_listen_sock < 0)
		return (-1);

	/*
	 * Add a file handler for the listen socket.  ndmp_select will call
	 * data_accept_session when a connection is ready to be accepted.
	 */
	if (ndmp_add_file_handler(session, session,
	    session->ns_data.dd_listen_sock, NDMPD_SELECT_MODE_READ, HC_MOVER,
	    data_accept_connection_v3) < 0) {
		(void) close(session->ns_data.dd_listen_sock);
		session->ns_data.dd_listen_sock = -1;
		return (-1);
	}

	ndmp_debug(session, "data listen socket address is %s:%d",
	    inet_ntoa(IN_ADDR(*addr)), ntohs(*port));

	return (0);
}


static const char *
ndmp_butype_valid(ndmp_session_t *session, const char *type)
{
	ndmp_server_conf_t *conf = session->ns_server->ns_conf;
	int i;

	for (i = 0; conf->ns_types[i] != NULL; i++) {
		if (strcasecmp(type, conf->ns_types[i]) == 0)
			return (conf->ns_types[i]);
	}

	return (NULL);
}

/*
 * Request handler. Returns the environment variable array sent with the backup
 * request. This request may only be sent with a backup operation is in
 * progress.
 */
/*ARGSUSED*/
void
ndmp_data_get_env_v3(ndmp_session_t *session, void *body)
{
	ndmp_data_get_env_reply reply = { 0 };

	(void) mutex_lock(&session->ns_data.dd_env_lock);

	if (session->ns_data.dd_operation != NDMP_DATA_OP_BACKUP) {
		ndmp_log(session, LOG_ERR, "backup operation not active");
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		reply.env.env_len = 0;
	} else {
		reply.env.env_len = session->ns_data.dd_env_len;
		reply.env.env_val = session->ns_data.dd_env;
	}

	ndmp_send_reply(session, &reply);

	(void) mutex_unlock(&session->ns_data.dd_env_lock);
}

/*
 * Request handler. Returns current data state.
 */
/*ARGSUSED*/
void
ndmp_data_get_state_v3(ndmp_session_t *session, void *body)
{
	ndmp_data_get_state_reply_v3 reply = { 0 };

	reply.invalid = NDMP_DATA_STATE_EST_BYTES_REMAIN_INVALID
	    | NDMP_DATA_STATE_EST_TIME_REMAIN_INVALID;
	reply.operation = session->ns_data.dd_operation;
	reply.state = session->ns_data.dd_state;
	reply.halt_reason = session->ns_data.dd_halt_reason;

	reply.bytes_processed =
	    long_long_to_quad(
	    session->ns_data.dd_bytes_processed);

	reply.est_bytes_remain = long_long_to_quad(0LL);
	reply.est_time_remain = 0;
	if (session->ns_data.dd_state != NDMP_DATA_STATE_IDLE)
		ndmp_copy_addr_v3(&reply.data_connection_addr,
		    &session->ns_data.dd_data_addr);
	reply.read_offset = long_long_to_quad(session->ns_data.dd_read_offset);
	reply.read_length = long_long_to_quad(session->ns_data.dd_read_length);

	ndmp_send_reply(session, &reply);
}

/*
 * Request handler. Starts a backup.
 */
void
ndmp_data_start_backup_v3(ndmp_session_t *session, void *body)
{
	ndmp_data_start_backup_request_v3 *request;
	ndmp_data_start_backup_reply_v3 reply = { 0 };
	ndmp_server_conf_t *conf = session->ns_server->ns_conf;
	const char *type;

	request = (ndmp_data_start_backup_request_v3 *)body;

	if (session->ns_data.dd_state != NDMP_DATA_STATE_CONNECTED) {
		ndmp_log(session, LOG_ERR,
		    "invalid data state for backup command");
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		goto error;
	}

	if (session->ns_data.dd_data_addr.addr_type == NDMP_ADDR_LOCAL) {
		if (session->ns_tape.td_mode == NDMP_TAPE_READ_MODE) {
			ndmp_log(session, LOG_ERR,
			    "write protected device");
			reply.error = NDMP_WRITE_PROTECT_ERR;
			goto error;
		}
	}

	if ((type = ndmp_butype_valid(session, request->bu_type)) == NULL) {
		ndmp_log(session, LOG_ERR, "invalid backup type '%s'",
		    request->bu_type);
		reply.error = NDMP_ILLEGAL_ARGS_ERR;
		goto error;
	}

	reply.error = ndmp_save_env(session, request->env.env_val,
	    request->env.env_len);

	if (reply.error != NDMP_NO_ERR)
		goto error;

	session->ns_data.dd_state = NDMP_DATA_STATE_ACTIVE;
	session->ns_data.dd_operation = NDMP_DATA_OP_BACKUP;

	assert(!session->ns_running);
	reply.error = conf->ns_start_backup(session, type);
	if (reply.error != NDMP_NO_ERR)
		goto error;
	session->ns_running = B_TRUE;

	(void) ndmp_send_response(session, NDMP_NO_ERR,
	    &reply);

	return;

error:
	assert(reply.error != NDMP_NO_ERR);
	ndmp_send_reply(session, &reply);
	ndmp_data_cleanup(session);
}

/*
 * Request handler. Starts a restore.
 */
void
ndmp_data_start_recover_v3(ndmp_session_t *session, void *body)
{
	ndmp_data_start_recover_request_v3 *request = body;
	ndmp_data_start_recover_reply_v3 reply = { 0 };
	ndmp_server_conf_t *conf = session->ns_server->ns_conf;
	const char *type;

	if (session->ns_data.dd_state != NDMP_DATA_STATE_CONNECTED) {
		ndmp_log(session, LOG_ERR,
		    "invalid state for recover command");
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		goto _error;
	}

	if ((type = ndmp_butype_valid(session, request->bu_type)) == NULL) {
		ndmp_log(session, LOG_ERR, "invalid backup type '%s'",
		    request->bu_type);
		reply.error = NDMP_ILLEGAL_ARGS_ERR;
		goto _error;
	}

	reply.error = ndmp_save_env(session, request->env.env_val,
	    request->env.env_len);
	if (reply.error != NDMP_NO_ERR)
		goto _error;

	reply.error = ndmp_save_nlist_v3(session, request->nlist.nlist_val,
	    request->nlist.nlist_len);
	if (reply.error != NDMP_NO_ERR)
		goto _error;

	assert(!session->ns_running);
	reply.error = conf->ns_start_recover(session, type);
	if (reply.error != NDMP_NO_ERR)
		goto _error;
	session->ns_running = B_TRUE;

	if (ndmp_send_response(session, NDMP_NO_ERR,
	    &reply) < 0) {
		ndmp_data_error(session, NDMP_DATA_HALT_CONNECT_ERROR);
	}
	return;

_error:
	assert(reply.error != NDMP_NO_ERR);
	ndmp_send_reply(session, &reply);
	ndmp_data_error(session, NDMP_DATA_HALT_INTERNAL_ERROR);
	ndmp_data_cleanup(session);
}

/*
 * Request handler. Aborts the current backup/restore. The operation
 * state is not changed to the halted state until after the operation
 * has actually been aborted and the notify_halt request has been sent.
 */
/*ARGSUSED*/
void
ndmp_data_abort_v3(ndmp_session_t *session, void *body)
{
	ndmp_data_abort_reply reply = { 0 };

	switch (session->ns_data.dd_state) {
	case NDMP_DATA_STATE_IDLE:
		ndmp_log(session, LOG_ERR,
		    "invalid state for abort request");
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		break;

	case NDMP_DATA_STATE_ACTIVE:
		/*
		 * Don't go to the HALTED state yet, just signal the running
		 * operation that it should abort, which will call the
		 * done method and notify the consumer that it's aborted.
		 */
		ndmp_session_data_stop(session);
		break;

	case NDMP_DATA_STATE_HALTED:
	case NDMP_DATA_STATE_LISTEN:
	case NDMP_DATA_STATE_CONNECTED:
		ndmp_data_error(session, NDMP_DATA_HALT_ABORTED);
		break;
	default:
		abort();
	}

	ndmp_send_reply(session, &reply);
}

/*
 * Request handler. Stops the current data operation.
 */
/*ARGSUSED*/
void
ndmp_data_stop_v3(ndmp_session_t *session, void *body)
{
	ndmp_data_stop_reply reply = { 0 };

	if (session->ns_data.dd_state != NDMP_DATA_STATE_HALTED) {
		ndmp_log(session, LOG_ERR,
		    "invalid data state for stop command");
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_send_reply(session, &reply);
		return;
	}

	ndmp_session_data_stop(session);

	ndmp_data_cleanup(session);

	/* prepare for another data operation */
	ndmp_data_init(session);

	ndmp_send_reply(session, &reply);
}

/*
 * Request handler.  Configures the server to listen for a session
 * from a remote mover.
 */
void
ndmp_data_listen_v3(ndmp_session_t *session, void *body)
{
	ndmp_data_listen_request_v3 *request = body;
	ndmp_data_listen_reply_v3 reply = { 0 };
	ulong_t addr;
	ushort_t port;

	if (session->ns_data.dd_state != NDMP_DATA_STATE_IDLE) {
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_log(session, LOG_ERR,
		    "invalid data state for listen command");
	} else if (session->ns_mover.md_state != NDMP_MOVER_STATE_IDLE) {
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_log(session, LOG_ERR,
		    "invalid mover state for listen command");
	}

	if (reply.error != NDMP_NO_ERR) {
		ndmp_send_reply(session, &reply);
		return;
	}

	switch (request->addr_type) {
	case NDMP_ADDR_LOCAL:
		reply.data_connection_addr.addr_type = request->addr_type;
		session->ns_data.dd_data_addr.addr_type = NDMP_ADDR_LOCAL;
		break;
	case NDMP_ADDR_TCP:
		if (create_listen_socket_v3(session, &addr, &port) < 0) {
			reply.error = NDMP_IO_ERR;
			break;
		}

		reply.data_connection_addr.addr_type = request->addr_type;
		reply.data_connection_addr.tcp_ip_v3 = htonl(addr);
		reply.data_connection_addr.tcp_port_v3 = htons(port);
		session->ns_data.dd_data_addr.addr_type = NDMP_ADDR_TCP;
		session->ns_data.dd_data_addr.tcp_ip_v3 = addr;
		session->ns_data.dd_data_addr.tcp_port_v3 = port;
		ndmp_debug(session, "data listening on socket %d",
		    session->ns_data.dd_listen_sock);
		break;

	default:
		ndmp_log(session, LOG_ERR,
		    "invalid address type 0x%x", request->addr_type);
		reply.error = NDMP_ILLEGAL_ARGS_ERR;
		break;
	}

	if (reply.error == NDMP_NO_ERR)
		session->ns_data.dd_state = NDMP_DATA_STATE_LISTEN;

	ndmp_send_reply(session, &reply);
}

/*
 * Request handler. Connects the data server to either a local
 * or remote mover.
 */
void
ndmp_data_connect_v3(ndmp_session_t *session, void *body)
{
	ndmp_data_connect_request_v3 *request = body;
	ndmp_data_connect_reply_v3 reply = { 0 };

	if (session->ns_data.dd_state != NDMP_DATA_STATE_IDLE) {
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_log(session, LOG_ERR,
		    "invalid data state for connect command");
		ndmp_send_reply(session, &reply);
		return;
	}

	switch (request->addr.addr_type) {
	case NDMP_ADDR_LOCAL:
		/*
		 * Verify that the mover is listening for a
		 * local session
		 */
		if (session->ns_mover.md_state != NDMP_MOVER_STATE_LISTEN ||
		    session->ns_mover.md_listen_sock != -1) {
			reply.error = NDMP_ILLEGAL_STATE_ERR;
			ndmp_log(session, LOG_ERR,
			    "invalid mover state for connect command");
		} else {
			session->ns_mover.md_state = NDMP_MOVER_STATE_ACTIVE;
		}
		break;

	case NDMP_ADDR_TCP:
		reply.error = data_connect_sock_v3(session,
		    request->addr.tcp_ip_v3, request->addr.tcp_port_v3);
		break;

	default:
		reply.error = NDMP_ILLEGAL_ARGS_ERR;
		ndmp_log(session, LOG_ERR, "invalid address type 0x%x",
		    request->addr.addr_type);
	}

	if (reply.error == NDMP_NO_ERR)
		session->ns_data.dd_state = NDMP_DATA_STATE_CONNECTED;

	ndmp_send_reply(session, &reply);
}

/*
 * Request handler. Returns the environment variable array sent with the backup
 * request. This request may only be sent when a backup operation is in
 * progress.
 */
/*ARGSUSED*/
void
ndmp_data_get_env_v4(ndmp_session_t *session, void *body)
{
	ndmp_data_get_env_reply reply = { 0 };

	if (session->ns_data.dd_state != NDMP_DATA_STATE_ACTIVE &&
	    session->ns_data.dd_state != NDMP_DATA_STATE_HALTED) {
		ndmp_log(session, LOG_ERR,
		    "invalid data state for get_env command");
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		reply.env.env_len = 0;
	} else if (session->ns_data.dd_operation != NDMP_DATA_OP_BACKUP) {
		ndmp_log(session, LOG_ERR, "backup operation not active");
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		reply.env.env_len = 0;
	} else {
		reply.env.env_len = session->ns_data.dd_env_len;
		reply.env.env_val = session->ns_data.dd_env;
	}

	ndmp_send_reply(session, &reply);
}

/*
 * Request handler.  Returns the current data state.
 */
/*ARGSUSED*/
void
ndmp_data_get_state_v4(ndmp_session_t *session, void *body)
{
	ndmp_data_get_state_reply_v4 reply = { 0 };

	reply.unsupported = NDMP_DATA_STATE_EST_BYTES_REMAIN_INVALID
	    | NDMP_DATA_STATE_EST_TIME_REMAIN_INVALID;
	reply.operation = session->ns_data.dd_operation;
	reply.state = session->ns_data.dd_state;
	reply.halt_reason = session->ns_data.dd_halt_reason;

	reply.bytes_processed = long_long_to_quad(
	    session->ns_data.dd_bytes_processed);

	reply.est_bytes_remain = long_long_to_quad(0LL);
	reply.est_time_remain = 0;
	if (session->ns_data.dd_state != NDMP_DATA_STATE_IDLE)
		(void) ndmp_copy_addr_v4(session, &reply.data_connection_addr,
		    &session->ns_data.dd_data_addr_v4);

	reply.read_offset = long_long_to_quad(session->ns_data.dd_read_offset);
	reply.read_length = long_long_to_quad(session->ns_data.dd_read_length);

	ndmp_send_reply(session, &reply);
	free(reply.data_connection_addr.tcp_addr_v4);
}

/*
 * Request handler. Connects the data server to either a local
 * or remote mover.
 */
void
ndmp_data_connect_v4(ndmp_session_t *session, void *body)
{
	ndmp_data_connect_request_v4 *request;
	ndmp_data_connect_reply_v4 reply = { 0 };

	request = (ndmp_data_connect_request_v4 *)body;

	if (session->ns_data.dd_state != NDMP_DATA_STATE_IDLE) {
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_log(session, LOG_ERR,
		    "invalid data state for connect command");
		ndmp_send_reply(session, &reply);
	}

	if (reply.error != NDMP_NO_ERR) {
		ndmp_send_reply(session, &reply);
		return;
	}

	switch (request->addr.addr_type) {
	case NDMP_ADDR_LOCAL:
		/*
		 * Verify that the mover is listening for a
		 * local session
		 */
		if (session->ns_mover.md_state != NDMP_MOVER_STATE_LISTEN ||
		    session->ns_mover.md_listen_sock != -1) {
			reply.error = NDMP_ILLEGAL_STATE_ERR;
			ndmp_log(session, LOG_ERR,
			    "invalid mover state for connect command");
		} else {
			session->ns_mover.md_state = NDMP_MOVER_STATE_ACTIVE;
		}
		break;

	case NDMP_ADDR_TCP:
		reply.error = data_connect_sock_v3(session,
		    request->addr.tcp_ip_v4(0), request->addr.tcp_port_v4(0));
		break;

	default:
		reply.error = NDMP_ILLEGAL_ARGS_ERR;
		ndmp_log(session, LOG_ERR, "invalid address type 0x%x",
		    request->addr.addr_type);
	}

	if (reply.error == NDMP_NO_ERR)
		session->ns_data.dd_state = NDMP_DATA_STATE_CONNECTED;

	ndmp_send_reply(session, &reply);
}

/*
 * Request handler.  Configures the server to listen for a session from a
 * remote mover.
 */
void
ndmp_data_listen_v4(ndmp_session_t *session, void *body)
{
	ndmp_data_listen_request_v4 *request = body;
	ndmp_data_listen_reply_v4 reply = { 0 };
	ulong_t addr;
	ushort_t port;

	if (session->ns_data.dd_state != NDMP_DATA_STATE_IDLE) {
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_log(session, LOG_ERR,
		    "invalid data state for listen command");
	} else if (session->ns_mover.md_state != NDMP_MOVER_STATE_IDLE) {
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_log(session, LOG_ERR,
		    "invalid mover state for listen command");
	}

	if (reply.error != NDMP_NO_ERR) {
		ndmp_send_reply(session, &reply);
		return;
	}

	switch (request->addr_type) {
	case NDMP_ADDR_LOCAL:
		reply.connect_addr.addr_type = request->addr_type;
		session->ns_data.dd_data_addr.addr_type = NDMP_ADDR_LOCAL;
		break;
	case NDMP_ADDR_TCP:
		if (create_listen_socket_v3(session, &addr, &port) < 0) {
			reply.error = NDMP_IO_ERR;
			break;
		}

		reply.connect_addr.addr_type = request->addr_type;
		reply.connect_addr.tcp_addr_v4 =
		    alloca(sizeof (ndmp_tcp_addr_v4));
		bzero(reply.connect_addr.tcp_addr_v4,
		    sizeof (ndmp_tcp_addr_v4));

		reply.connect_addr.tcp_ip_v4(0) = htonl(addr);
		reply.connect_addr.tcp_port_v4(0) = htons(port);
		reply.connect_addr.tcp_len_v4 = 1;

		session->ns_data.dd_data_addr_v4.addr_type = NDMP_ADDR_TCP;
		session->ns_data.dd_data_addr_v4.tcp_addr_v4 =
		    ndmp_malloc(session, sizeof (ndmp_tcp_addr_v4));

		session->ns_data.dd_data_addr_v4.tcp_ip_v4(0) = addr;
		session->ns_data.dd_data_addr_v4.tcp_port_v4(0) = port;
		session->ns_data.dd_data_addr_v4.tcp_len_v4 = 1;

		/* Copy that to data_addr for compatibility */
		session->ns_data.dd_data_addr.addr_type = NDMP_ADDR_TCP;
		session->ns_data.dd_data_addr.tcp_ip_v3 = addr;
		session->ns_data.dd_data_addr.tcp_port_v3 = port;
		ndmp_debug(session, "data listen socket is %d",
		    session->ns_data.dd_listen_sock);
		break;

	default:
		ndmp_log(session, LOG_ERR, "invalid address type 0x%x",
		    request->addr_type);
		reply.error = NDMP_ILLEGAL_ARGS_ERR;
		break;
	}

	if (reply.error == NDMP_NO_ERR)
		session->ns_data.dd_state = NDMP_DATA_STATE_LISTEN;

	ndmp_send_reply(session, &reply);
}

/*
 * This function sends the notification message to the client when the data
 * service is halted.
 */
/*ARGSUSED*/
static void
ndmp_data_error_send(ndmp_session_t *session, ndmp_data_halt_reason reason)
{
	ndmp_notify_data_halted_request req;

	req.reason = session->ns_data.dd_halt_reason;
	req.text_reason = "";

	/*
	 * The V4 version of this request doesn't have the reason, but we can
	 * use the XDR routines to encode the reason.
	 */
	(void) ndmp_send_request(session, NDMP_NOTIFY_DATA_HALTED,
	    &req, NULL);
}

/*
 * This function is called when a data error has been detected.  A notify
 * message is sent to the client and the data server is placed into the halted
 * state.
 */
void
ndmp_data_error(ndmp_session_t *session, ndmp_data_halt_reason reason)
{
	if (session->ns_data.dd_state == NDMP_DATA_STATE_IDLE ||
	    session->ns_data.dd_state == NDMP_DATA_STATE_HALTED)
		return;

	if (session->ns_data.dd_operation == NDMP_DATA_OP_BACKUP) {
		/*
		 * If mover local and successful backup, write any
		 * remaining buffered data to tape.
		 */
		if (session->ns_data.dd_data_addr.addr_type ==
		    NDMP_ADDR_LOCAL && reason == NDMP_DATA_HALT_SUCCESSFUL) {
			(void) ndmp_local_write_v3(session, 0, 0);
		}
	}

	session->ns_data.dd_state = NDMP_DATA_STATE_HALTED;
	session->ns_data.dd_halt_reason = reason;

	ndmp_data_error_send(session, reason);

	ndmp_debug(session, "data error reason 0x%x, closing connection",
	    reason);

	if (session->ns_data.dd_data_addr.addr_type == NDMP_ADDR_TCP) {
		if (session->ns_data.dd_sock != -1) {
			ndmp_remove_file_handler(session,
			    session->ns_data.dd_sock);
			(void) close(session->ns_data.dd_sock);
			session->ns_data.dd_sock = -1;
		}
		if (session->ns_data.dd_listen_sock != -1) {
			ndmp_remove_file_handler(session,
			    session->ns_data.dd_listen_sock);

			(void) close(session->ns_data.dd_listen_sock);
			session->ns_data.dd_listen_sock = -1;
		}
	} else {
		ndmp_mover_error(session, NDMP_MOVER_HALT_CONNECT_CLOSED);
	}
}

/*
 * Read and discard data from the data session.  Called when a module has
 * called ndmp_seek() on a remote data source prior to reading all of the data
 * from the previous seek.
 */
static int
discard_data_v3(ndmp_session_t *session, ulong_t length)
{
	static char buf[MAX_RECORD_SIZE];
	int n, toread;

	toread = (length < MAX_RECORD_SIZE) ? length :
	    MAX_RECORD_SIZE;

	/* Read and discard the data. */
	n = read(session->ns_data.dd_sock, buf, toread);
	if (n < 0) {
		session->ns_data.dd_errno = errno;
		ndmp_log(session, LOG_ERR, "failed to read from session: %s",
		    strerror(errno));
		n = -1;
	}

	return (n);
}

/*
 * Reads data from the remote mover.  Returns the number of bytes read, or -1
 * on error.
 */
int
ndmp_remote_read_v3(ndmp_session_t *session, char *data, ssize_t length)
{
	int count;
	int len;
	ssize_t n;
	ndmp_notify_data_read_request request;

	ndmp_debug(session, "reading %lu bytes at offset %llu",
	    length, session->ns_data.dd_position);
	ndmp_debug(session, "ns_data.dd_xx: [%llu, %llu, %llu, %llu, %llu]",
	    session->ns_data.dd_bytes_left_to_read,
	    session->ns_data.dd_read_offset,
	    session->ns_data.dd_read_length,
	    session->ns_data.dd_position,
	    session->ns_data.dd_discard_length);

	count = 0;
	while (count < length) {
		len = length - count;

		/*
		 * If the end of the seek window has been reached then send an
		 * ndmp_read request to the client.  The NDMP client will then
		 * send a mover_data_read request to the remote mover and the
		 * mover will send more data.  This condition can occur if the
		 * module attempts to read past a seek window set via a prior
		 * call to ndmp_seek() or the module has not issued a seek. If
		 * no seek was issued then pretend that a seek was issued to
		 * read the entire tape.
		 */
		if (session->ns_data.dd_bytes_left_to_read == 0) {
			if (session->ns_data.dd_read_length == 0) {
				/* ndmp_seek() was never called */
				session->ns_data.dd_bytes_left_to_read = ~0LL;
				session->ns_data.dd_read_offset = 0LL;
				session->ns_data.dd_read_length = ~0LL;
			} else {
				/*
				 * Consumer issued a seek but is now trying to
				 * read beyond the end of the window.  In this
				 * scenario, continue to move the window in
				 * request-sized chunks.  Consumers should
				 * issue proper seek requests to ensure the
				 * best performance.
				 */
				session->ns_data.dd_bytes_left_to_read = len;
				session->ns_data.dd_read_offset =
				    session->ns_data.dd_position;
				session->ns_data.dd_read_length = len;
			}

			request.offset =
			    long_long_to_quad(session->ns_data.dd_read_offset);
			request.length =
			    long_long_to_quad(session->ns_data.dd_read_length);

			ndmp_debug(session, "to NOTIFY_DATA_READ [%llu, %llu]",
			    session->ns_data.dd_read_offset,
			    session->ns_data.dd_read_length);

			if (ndmp_send_request(session,
			    NDMP_NOTIFY_DATA_READ, &request, NULL) < 0) {
				return (-1);
			}
		}

		/*
		 * If the module called ndmp_seek() prior to reading all of
		 * the data that the remote mover was requested to send, then
		 * the excess data from the seek has to be discarded.
		 */
		if (session->ns_data.dd_discard_length != 0) {
			n = discard_data_v3(session,
			    (ulong_t)session->ns_data.dd_discard_length);
			if (n < 0)
				return (-1);

			session->ns_data.dd_discard_length -= n;
			continue;
		}

		/*
		 * Don't attempt to read more data than the remote is sending.
		 */
		if (len > session->ns_data.dd_bytes_left_to_read)
			len = session->ns_data.dd_bytes_left_to_read;

		if ((n = read(session->ns_data.dd_sock, &data[count],
		    len)) < 0) {
			session->ns_data.dd_errno = errno;
			ndmp_log(session, LOG_ERR,
			    "failed to read from data session: %s",
			    strerror(errno));
			return (-1);
		}

		/* read returns 0 if the connection was closed */
		if (n == 0) {
			ndmp_debug(session, "EOF seen when reading data");
			break;
		}

		count += n;
		session->ns_data.dd_bytes_left_to_read -= n;
		session->ns_data.dd_position += n;
	}

	return (count);
}

/*
 * Initializes data specific session variables.  This is called both when
 * creating a new session and after stopping an individual data operation, as
 * there can be mutliple data operations within one session.
 */
void
ndmp_data_init(ndmp_session_t *session)
{
	ndmp_debug(session, "initializing data");
	session->ns_data.dd_operation = NDMP_DATA_OP_NOACTION;
	session->ns_data.dd_state = NDMP_DATA_STATE_IDLE;
	session->ns_data.dd_halt_reason = NDMP_DATA_HALT_NA;
	session->ns_data.dd_env = 0;
	session->ns_data.dd_env_len = 0;
	session->ns_data.dd_nlist = 0;
	session->ns_data.dd_nlist_len = 0;
	session->ns_data.dd_mover.addr_type = NDMP_ADDR_LOCAL;
	session->ns_data.dd_sock = -1;
	session->ns_data.dd_read_offset = 0;
	session->ns_data.dd_read_length = 0;
	session->ns_data.dd_est_bytes_remaining = 0;
	session->ns_data.dd_est_time_remaining = 0;
	session->ns_data.dd_bytes_processed = 0;
	session->ns_data.dd_abort = B_FALSE;

	session->ns_data.dd_state = NDMP_DATA_STATE_IDLE;
	session->ns_data.dd_nlist_v3 = 0;
	session->ns_data.dd_data_addr.addr_type = NDMP_ADDR_LOCAL;
	session->ns_data.dd_listen_sock = -1;
	session->ns_data.dd_bytes_left_to_read = 0LL;
	session->ns_data.dd_position = 0LL;
	session->ns_data.dd_discard_length = 0LL;
	session->ns_data.dd_data_size = 0;
}

/*
 * Releases resources allocated during a data operation.
 */
void
ndmp_data_cleanup(ndmp_session_t *session)
{
	if (session->ns_data.dd_listen_sock != -1) {
		ndmp_debug(session, "closing data listen socket: %d",
		    session->ns_data.dd_listen_sock);
		ndmp_remove_file_handler(session,
		    session->ns_data.dd_listen_sock);
		(void) close(session->ns_data.dd_listen_sock);
		session->ns_data.dd_listen_sock = -1;
	}

	if (session->ns_data.dd_sock != -1) {
		ndmp_debug(session, "closing data socket: %d",
		    session->ns_data.dd_sock);
		(void) close(session->ns_data.dd_sock);
		session->ns_data.dd_sock = -1;
	}

	ndmp_free_nlist(session);
}
