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

/*
 * Maximum mover record size
 */
#define	MAX_MOVER_RECSIZE	(512*KILOBYTE)

static boolean_t is_writer_running_v3(ndmp_session_t *session);
static int mover_pause_v3(ndmp_session_t *session,
    ndmp_mover_pause_reason reason);
static int mover_tape_write_v3(ndmp_session_t *session, char *data,
    ssize_t length);
static int mover_tape_flush_v3(ndmp_session_t *session);
static int mover_tape_read_v3(ndmp_session_t *session, char *data);
static int create_listen_socket_v3(ndmp_session_t *session, ulong_t *addr,
    ushort_t *port);
static void mover_data_read_v3(ndmp_session_t *session, int fd, ulong_t mode);
static void mover_data_write_v3(ndmp_session_t *session, int fd, ulong_t mode);
static void accept_connection_v3(ndmp_session_t *session, int fd, ulong_t mode);
static ndmp_error mover_connect_sock_v3(ndmp_session_t *session,
    ndmp_mover_mode mode, ulong_t addr, ushort_t port);


int ndmp_max_mover_recsize = MAX_MOVER_RECSIZE; /* patchable */

#define	TAPE_READ_ERR		-1
#define	TAPE_NO_WRITER_ERR	-2

/*
 * This handler handles mover_stop requests.
 */
/*ARGSUSED*/
void
ndmp_mover_stop_v3(ndmp_session_t *session, void *body)
{
	ndmp_mover_stop_reply reply = { 0 };

	if (session->ns_mover.md_state != NDMP_MOVER_STATE_HALTED) {
		ndmp_log(session, LOG_ERR,
		    "invalid mover state for stop command");

		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_send_reply(session, &reply);
		return;
	}

	ndmp_session_data_stop(session);
	ndmp_send_reply(session, &reply);

	ndmp_mover_cleanup(session);
	(void) ndmp_mover_init(session);
}

/*
 * This handler handles mover_close requests.
 */
/*ARGSUSED*/
void
ndmp_mover_close_v3(ndmp_session_t *session, void *body)
{
	ndmp_mover_close_reply reply = { 0 };

	if (session->ns_mover.md_state != NDMP_MOVER_STATE_PAUSED) {
		ndmp_log(session, LOG_ERR,
		    "invalid mover state for close command");

		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_send_reply(session, &reply);
		return;
	}
	free(session->ns_mover.md_data_addr_v4.tcp_addr_v4);

	ndmp_send_reply(session, &reply);

	ndmp_mover_error(session, NDMP_MOVER_HALT_CONNECT_CLOSED);
}

/*
 * This handler handles the ndmp_mover_get_state_request.  Status information
 * for the mover state machine is returned.
 */
/*ARGSUSED*/
void
ndmp_mover_get_state_v3(ndmp_session_t *session, void *body)
{
	ndmp_mover_get_state_reply_v3 reply = { 0 };

	reply.state = session->ns_mover.md_state;
	reply.pause_reason = session->ns_mover.md_pause_reason;
	reply.halt_reason = session->ns_mover.md_halt_reason;
	reply.record_size = session->ns_mover.md_record_size;
	reply.record_num = session->ns_mover.md_record_num;
	reply.data_written =
	    long_long_to_quad(session->ns_mover.md_data_written);
	reply.seek_position =
	    long_long_to_quad(session->ns_mover.md_seek_position);
	reply.bytes_left_to_read =
	    long_long_to_quad(session->ns_mover.md_bytes_left_to_read);
	reply.window_offset =
	    long_long_to_quad(session->ns_mover.md_window_offset);
	reply.window_length =
	    long_long_to_quad(session->ns_mover.md_window_length);
	if (session->ns_mover.md_state != NDMP_MOVER_STATE_IDLE)
		ndmp_copy_addr_v3(&reply.data_connection_addr,
		    &session->ns_mover.md_data_addr);

	ndmp_send_reply(session, &reply);
}

/*
 * Validate listen parameters common to v3 and v4.
 */
static int
ndmp_mover_listen_validate(ndmp_session_t *session, int mode,
    int addr_type)
{
	if (mode != NDMP_MOVER_MODE_READ &&
	    mode != NDMP_MOVER_MODE_WRITE) {
		ndmp_log(session, LOG_ERR,
		    "invalid listen mode 0x%x", mode);
		return (NDMP_ILLEGAL_ARGS_ERR);
	} else if (session->ns_mover.md_state != NDMP_MOVER_STATE_IDLE) {
		ndmp_log(session, LOG_ERR,
		    "invalid mover state for listen command");
		return (NDMP_ILLEGAL_STATE_ERR);
	} else if (addr_type != NDMP_ADDR_LOCAL &&
	    addr_type != NDMP_ADDR_TCP) {
		ndmp_log(session, LOG_ERR,
		    "invalid address type 0x%x", addr_type);
		return (NDMP_ILLEGAL_ARGS_ERR);
	} else if (session->ns_data.dd_state != NDMP_DATA_STATE_IDLE) {
		ndmp_log(session, LOG_ERR,
		    "invalid data state for listen command");
		return (NDMP_ILLEGAL_STATE_ERR);
	} else if (session->ns_tape.td_fd == -1) {
		ndmp_log(session, LOG_ERR,
		    "no tape device open");
		return (NDMP_DEV_NOT_OPEN_ERR);
	} else if (mode == NDMP_MOVER_MODE_READ &&
	    session->ns_tape.td_mode == NDMP_TAPE_READ_MODE) {
		ndmp_log(session, LOG_ERR,
		    "write protected device");
		return (NDMP_PERMISSION_ERR);
	} else if (session->ns_version == NDMPV4 &&
	    session->ns_mover.md_record_size == 0) {
		ndmp_log(session, LOG_ERR,
		    "invalid record size 0");
		return (NDMP_PRECONDITION_ERR);
	}

	return (NDMP_NO_ERR);
}

/*
 * This handler handles ndmp_mover_listen_requests.  A TCP/IP socket is created
 * that is used to listen for and accept data sessions initiated by a remote
 * data server.
 */
void
ndmp_mover_listen_v3(ndmp_session_t *session, void *body)
{
	ndmp_mover_listen_request_v3 *request = body;
	ndmp_mover_listen_reply_v3 reply = { 0 };
	ulong_t addr;
	ushort_t port;

	reply.error = ndmp_mover_listen_validate(session,
	    request->mode, request->addr_type);

	if (reply.error != NDMP_NO_ERR) {
		ndmp_send_reply(session, &reply);
		return;
	}

	if (request->addr_type == NDMP_ADDR_LOCAL) {
		reply.data_connection_addr.addr_type = NDMP_ADDR_LOCAL;
		session->ns_mover.md_data_addr.addr_type = NDMP_ADDR_LOCAL;
	} else {
		assert(request->addr_type == NDMP_ADDR_TCP);
		if (create_listen_socket_v3(session, &addr, &port) < 0) {
			reply.error = NDMP_IO_ERR;
			goto error;
		}
		reply.data_connection_addr.addr_type = NDMP_ADDR_TCP;
		reply.data_connection_addr.tcp_ip_v3 = htonl(addr);
		reply.data_connection_addr.tcp_port_v3 = htons(port);
		session->ns_mover.md_data_addr.addr_type = NDMP_ADDR_TCP;
		session->ns_mover.md_data_addr.tcp_ip_v3 = addr;
		session->ns_mover.md_data_addr.tcp_port_v3 = ntohs(port);
		ndmp_debug(session, "mover listening on socket %d",
		    session->ns_mover.md_listen_sock);
	}

error:
	if (reply.error == NDMP_NO_ERR) {
		session->ns_mover.md_mode = request->mode;
		session->ns_mover.md_state = NDMP_MOVER_STATE_LISTEN;
	}

	ndmp_send_reply(session, &reply);
}

/*
 * This handler handles ndmp_mover_continue_requests.
 */
/*ARGSUSED*/
void
ndmp_mover_continue_v3(ndmp_session_t *session, void *body)
{
	ndmp_mover_continue_reply reply = { 0 };
	int ret;

	if (session->ns_mover.md_state != NDMP_MOVER_STATE_PAUSED) {
		ndmp_log(session, LOG_ERR,
		    "invalid mover state for continue command");
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_send_reply(session, &reply);
		return;
	}

	if (session->ns_version == NDMPV4 &&
	    !session->ns_mover.md_pre_cond) {
		reply.error = NDMP_PRECONDITION_ERR;
		ndmp_log(session, LOG_ERR,
		    "precondition check failed");
		ndmp_send_reply(session, &reply);
		return;
	}

	/*
	 * Restore the file handler if the mover is remote to the data
	 * server and the handler was removed pending the continuation of a
	 * seek request. The handler is removed in mover_data_write().
	 */
	if (session->ns_mover.md_pause_reason == NDMP_MOVER_PAUSE_SEEK &&
	    session->ns_mover.md_sock != -1) {
		/*
		 * If we are here, it means that we needed DMA interference for
		 * seek. We should be on the right window, so we do not need
		 * the DMA interference anymore.  We do another seek inside the
		 * Window to move to the exact position on the tape.  If the
		 * resore is running without DAR the pause reason should not be
		 * seek.
		 */
		ret = ndmp_mover_seek(session,
		    session->ns_mover.md_seek_position,
		    session->ns_mover.md_bytes_left_to_read);
		if (ret < 0) {
			ndmp_mover_error(session,
			    NDMP_MOVER_HALT_INTERNAL_ERROR);
			return;
		}

		if (!ret) {
			if (ndmp_add_file_handler(session, session,
			    session->ns_mover.md_sock, NDMPD_SELECT_MODE_WRITE,
			    HC_MOVER, mover_data_write_v3) < 0)
				ndmp_mover_error(session,
				    NDMP_MOVER_HALT_INTERNAL_ERROR);
		} else {
			/*
			 * This should not happen because we should be in the
			 * right window. This means that DMA does not follow
			 * the V3 spec.
			 */
			ndmp_log(session, LOG_ERR,
			    "invalid window state for continue command");
			ndmp_mover_error(session,
			    NDMP_MOVER_HALT_INTERNAL_ERROR);
			return;
		}
	}

	session->ns_mover.md_state = NDMP_MOVER_STATE_ACTIVE;
	session->ns_mover.md_pause_reason = NDMP_MOVER_PAUSE_NA;

	ndmp_send_reply(session, &reply);
}

/*
 * This handler handles mover_abort requests.
 */
/*ARGSUSED*/
void
ndmp_mover_abort_v3(ndmp_session_t *session, void *body)
{
	ndmp_mover_abort_reply reply = { 0 };

	if (session->ns_mover.md_state == NDMP_MOVER_STATE_IDLE ||
	    session->ns_mover.md_state == NDMP_MOVER_STATE_HALTED) {
		ndmp_log(session, LOG_ERR,
		    "invalid moer state for abort command");

		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_send_reply(session, &reply);
		return;
	}

	ndmp_send_reply(session, &reply);

	ndmp_mover_error(session, NDMP_MOVER_HALT_ABORTED);
}

/*
 * This handler handles mover_set_window requests.
 */
void
ndmp_mover_set_window_v3(ndmp_session_t *session, void *body)
{
	ndmp_mover_set_window_request *request;
	ndmp_mover_set_window_reply reply = { 0 };

	request = (ndmp_mover_set_window_request *) body;

	/*
	 * Note: The spec says that the window can be set only in the listen
	 * and paused states.  We let this happen when mover is in the idle
	 * state as well.  I can't rememebr which NDMP client (net_backup 4.5
	 * or net_worker 6.1.1) forced us to do this!
	 */
	if (session->ns_mover.md_state != NDMP_MOVER_STATE_IDLE &&
	    session->ns_mover.md_state != NDMP_MOVER_STATE_LISTEN &&
	    session->ns_mover.md_state != NDMP_MOVER_STATE_PAUSED) {
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_log(session, LOG_ERR,
		    "invalid mover state for set window command");
	} else if (session->ns_mover.md_record_size == 0) {
		if (session->ns_version == NDMPV4)
			reply.error = NDMP_PRECONDITION_ERR;
		else
			reply.error = NDMP_ILLEGAL_ARGS_ERR;
		ndmp_log(session, LOG_ERR,
		    "invalid record size 0");
	}

	if (quad_to_long_long(request->length) == 0) {
		reply.error = NDMP_ILLEGAL_ARGS_ERR;
		ndmp_log(session, LOG_ERR,
		    "invalid window size 0");
	}

	if (reply.error != NDMP_NO_ERR) {
		ndmp_send_reply(session, &reply);
		return;
	}

	session->ns_mover.md_pre_cond = B_TRUE;
	session->ns_mover.md_window_offset = quad_to_long_long(request->offset);
	session->ns_mover.md_window_length = quad_to_long_long(request->length);

	ndmp_debug(session, "mover window = [%llu, %llu]",
	    session->ns_mover.md_window_offset,
	    session->ns_mover.md_window_length);

	/*
	 * We have to update the position for DAR. DAR needs this
	 * information to position to the right index on tape,
	 * especially when we span the tapes.
	 */
	session->ns_mover.md_position =
	    session->ns_mover.md_window_offset;

	ndmp_send_reply(session, &reply);
}

/*
 * This handler handles ndmp_mover_read_requests.  If the requested offset is
 * outside of the current window, the mover is paused and a notify_mover_paused
 * request is sent notifying the client that a seek is required. If the
 * requested offest is within the window but not within the current record,
 * then the tape is positioned to the record containing the requested offest.
 * The requested amount of data is then read from the tape device and written
 * to the data session.
 */
void
ndmp_mover_read_v3(ndmp_session_t *session, void *body)
{
	ndmp_mover_read_request *request = (ndmp_mover_read_request *)body;
	ndmp_mover_read_reply reply = { 0 };
	int err;

	if (session->ns_mover.md_state != NDMP_MOVER_STATE_ACTIVE ||
	    session->ns_mover.md_mode != NDMP_MOVER_MODE_WRITE) {
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_log(session, LOG_ERR,
		    "invalid mover state for read command");
	} else if (session->ns_mover.md_bytes_left_to_read != 0) {
		reply.error = NDMP_READ_IN_PROGRESS_ERR;
		ndmp_log(session, LOG_ERR,
		    "read already in progres");
	} else if (session->ns_tape.td_fd == -1) {
		reply.error = NDMP_DEV_NOT_OPEN_ERR;
		ndmp_log(session, LOG_ERR,
		    "tape device is not open");
	} else if (quad_to_long_long(request->length) == 0 ||
	    (quad_to_long_long(request->length) == MAX_WINDOW_SIZE &&
	    quad_to_long_long(request->offset) != 0)) {
		reply.error = NDMP_ILLEGAL_ARGS_ERR;
		ndmp_log(session, LOG_ERR,
		    "invalid parameters for read command");
	}

	ndmp_send_reply(session, &reply);
	if (reply.error != NDMP_NO_ERR)
		return;

	err = ndmp_mover_seek(session, quad_to_long_long(request->offset),
	    quad_to_long_long(request->length));
	if (err < 0) {
		ndmp_mover_error(session, NDMP_MOVER_HALT_INTERNAL_ERROR);
		return;
	}

	/*
	 * Just return if we are waiting for the DMA to complete the seek.
	 */
	if (err == 1)
		return;

	/*
	 * Setup a handler function that will be called when
	 * data can be written to the data connection without blocking.
	 */
	if (ndmp_add_file_handler(session, (void*)session,
	    session->ns_mover.md_sock, NDMPD_SELECT_MODE_WRITE, HC_MOVER,
	    mover_data_write_v3) < 0) {
		ndmp_mover_error(session, NDMP_MOVER_HALT_INTERNAL_ERROR);
		return;
	}
}

/*
 * This handler handles mover_set_record_size requests.
 */
void
ndmp_mover_set_record_size_v3(ndmp_session_t *session, void *body)
{
	ndmp_mover_set_record_size_request *request;
	ndmp_mover_set_record_size_reply reply = { 0 };
	char *cp;

	request = (ndmp_mover_set_record_size_request *) body;

	if (session->ns_mover.md_state != NDMP_MOVER_STATE_IDLE) {
		reply.error = NDMP_ILLEGAL_STATE_ERR;
		ndmp_log(session, LOG_ERR,
		    "invalid mover state for set record size command");
	} else if (request->len > (unsigned int)ndmp_max_mover_recsize) {
		reply.error = NDMP_ILLEGAL_ARGS_ERR;
		ndmp_log(session, LOG_ERR,
		    "record size request greater than maximum size");
	} else if (request->len != session->ns_mover.md_record_size) {
		cp = ndmp_realloc(session, session->ns_mover.md_buf,
		    request->len);
		if (cp == NULL) {
			reply.error = NDMP_NO_MEM_ERR;
		} else {
			session->ns_mover.md_buf = cp;
			session->ns_mover.md_record_size = request->len;
			session->ns_mover.md_window_offset = 0;
			session->ns_mover.md_window_length = 0;
		}
	}

	ndmp_send_reply(session, &reply);
}

/*
 * Validate connect parameters common to v3 and v4.
 */
static int
ndmp_connect_validate(ndmp_session_t *session, int mode,
    int addr_type)
{
	if (mode != NDMP_MOVER_MODE_READ &&
	    mode != NDMP_MOVER_MODE_WRITE) {
		ndmp_log(session, LOG_ERR,
		    "invalid connect mode 0x%x", mode);
		return (NDMP_ILLEGAL_ARGS_ERR);
	} else if (session->ns_mover.md_state != NDMP_MOVER_STATE_IDLE) {
		ndmp_log(session, LOG_ERR,
		    "invalid mover state for listen command");
		return (NDMP_ILLEGAL_STATE_ERR);
	} else if (addr_type != NDMP_ADDR_LOCAL &&
	    addr_type != NDMP_ADDR_TCP) {
		ndmp_log(session, LOG_ERR,
		    "invalid address type 0x%x", addr_type);
		return (NDMP_ILLEGAL_ARGS_ERR);
	} else if (session->ns_data.dd_state != NDMP_DATA_STATE_IDLE) {
		ndmp_log(session, LOG_ERR,
		    "invalid data state for listen command");
		return (NDMP_ILLEGAL_STATE_ERR);
	} else if (session->ns_tape.td_fd == -1) {
		ndmp_log(session, LOG_ERR,
		    "no tape device open");
		return (NDMP_DEV_NOT_OPEN_ERR);
	} else if (mode == NDMP_MOVER_MODE_READ &&
	    session->ns_tape.td_mode == NDMP_TAPE_READ_MODE) {
		ndmp_log(session, LOG_ERR,
		    "write protected device");
		return (NDMP_PERMISSION_ERR);
	} else if (session->ns_version == NDMPV4 &&
	    session->ns_mover.md_record_size == 0) {
		ndmp_log(session, LOG_ERR,
		    "invalid record size 0");
		return (NDMP_PRECONDITION_ERR);
	}


	return (NDMP_NO_ERR);
}

/*
 * Request handler. Connects the mover to either a local or remote data server.
 */
void
ndmp_mover_connect_v3(ndmp_session_t *session, void *body)
{
	ndmp_mover_connect_request_v3 *request;
	ndmp_mover_connect_reply_v3 reply;

	request = (ndmp_mover_connect_request_v3*)body;

	reply.error = ndmp_connect_validate(session, request->mode,
	    request->addr.addr_type);

	if (reply.error != NDMP_NO_ERR) {
		ndmp_send_reply(session, &reply);
		return;
	}

	if (request->addr.addr_type == NDMP_ADDR_LOCAL) {
		/*
		 * Verify that the data server is listening for a
		 * local session.
		 */
		if (session->ns_data.dd_state != NDMP_DATA_STATE_LISTEN ||
		    session->ns_data.dd_listen_sock != -1) {
			ndmp_log(session, LOG_ERR,
			    "invalid data state for connect command");
			reply.error = NDMP_ILLEGAL_STATE_ERR;
		} else {
			session->ns_data.dd_state = NDMP_DATA_STATE_CONNECTED;
		}
	} else {
		assert(request->addr.addr_type == NDMP_ADDR_TCP);
	}

	if (reply.error == NDMP_NO_ERR) {
		session->ns_mover.md_data_addr.addr_type =
		    request->addr.addr_type;
		session->ns_mover.md_state = NDMP_MOVER_STATE_ACTIVE;
		session->ns_mover.md_mode = request->mode;
	}

	ndmp_send_reply(session, &reply);
}

/*
 * This handler handles the ndmp_mover_get_state_request.  Status information
 * for the mover state machine is returned.
 */
/*ARGSUSED*/
void
ndmp_mover_get_state_v4(ndmp_session_t *session, void *body)
{
	ndmp_mover_get_state_reply_v4 reply = { 0 };

	reply.state = session->ns_mover.md_state;
	reply.mode = session->ns_mover.md_mode;
	reply.pause_reason = session->ns_mover.md_pause_reason;
	reply.halt_reason = session->ns_mover.md_halt_reason;
	reply.record_size = session->ns_mover.md_record_size;
	reply.record_num = session->ns_mover.md_record_num;
	reply.bytes_moved =
	    long_long_to_quad(session->ns_mover.md_data_written);
	reply.seek_position =
	    long_long_to_quad(session->ns_mover.md_seek_position);
	reply.bytes_left_to_read =
	    long_long_to_quad(session->ns_mover.md_bytes_left_to_read);
	reply.window_offset =
	    long_long_to_quad(session->ns_mover.md_window_offset);
	reply.window_length =
	    long_long_to_quad(session->ns_mover.md_window_length);
	if (session->ns_mover.md_state != NDMP_MOVER_STATE_IDLE) {
		if (ndmp_copy_addr_v4(session, &reply.data_connection_addr,
		    &session->ns_mover.md_data_addr_v4) != 0) {
			reply.error = NDMP_NO_MEM_ERR;
		}
	}

	ndmp_send_reply(session, &reply);
	free(reply.data_connection_addr.tcp_addr_v4);
}

/*
 * This handler handles ndmp_mover_listen_requests.  A TCP/IP socket is created
 * that is used to listen for and accept data sessions initiated by a remote
 * data server.
 */
void
ndmp_mover_listen_v4(ndmp_session_t *session, void *body)
{
	ndmp_mover_listen_request_v4 *request;
	ndmp_mover_listen_reply_v4 reply = { 0 };
	ulong_t addr;
	ushort_t port;

	request = (ndmp_mover_listen_request_v4 *)body;

	reply.error = ndmp_mover_listen_validate(session,
	    request->mode, request->addr_type);

	if (reply.error != NDMP_NO_ERR) {
		ndmp_send_reply(session, &reply);
		return;
	}

	if (request->addr_type == NDMP_ADDR_LOCAL) {
		reply.connect_addr.addr_type = NDMP_ADDR_LOCAL;
		session->ns_mover.md_data_addr.addr_type = NDMP_ADDR_LOCAL;
	} else {
		assert(request->addr_type == NDMP_ADDR_TCP);
		if (create_listen_socket_v3(session, &addr, &port) < 0) {
			reply.error = NDMP_IO_ERR;
			goto error;
		}

		session->ns_mover.md_data_addr_v4.addr_type = NDMP_ADDR_TCP;
		session->ns_mover.md_data_addr_v4.tcp_len_v4 = 1;
		session->ns_mover.md_data_addr_v4.tcp_addr_v4 =
		    ndmp_malloc(session, sizeof (ndmp_tcp_addr_v4));

		session->ns_mover.md_data_addr_v4.tcp_ip_v4(0) = addr;
		session->ns_mover.md_data_addr_v4.tcp_port_v4(0) = ntohs(port);

		if (ndmp_copy_addr_v4(session, &reply.connect_addr,
		    &session->ns_mover.md_data_addr_v4) != 0) {
			reply.error = NDMP_NO_MEM_ERR;
		}

		/* For compatibility with V3 */
		session->ns_mover.md_data_addr.addr_type = NDMP_ADDR_TCP;
		session->ns_mover.md_data_addr.tcp_ip_v3 = addr;
		session->ns_mover.md_data_addr.tcp_port_v3 = ntohs(port);
		ndmp_debug(session, "listen socket: %d",
		    session->ns_mover.md_listen_sock);
	}

error:
	if (reply.error == NDMP_NO_ERR) {
		session->ns_mover.md_mode = request->mode;
		session->ns_mover.md_state = NDMP_MOVER_STATE_LISTEN;
	}

	ndmp_send_reply(session, &reply);
	free(reply.connect_addr.tcp_addr_v4);
}

/*
 * Request handler. Connects the mover to either a local or remote data server.
 */
void
ndmp_mover_connect_v4(ndmp_session_t *session, void *body)
{
	ndmp_mover_connect_request_v4 *request;
	ndmp_mover_connect_reply_v4 reply = { 0 };

	request = (ndmp_mover_connect_request_v4 *)body;

	reply.error = ndmp_connect_validate(session, request->mode,
	    request->addr.addr_type);

	if (reply.error != NDMP_NO_ERR) {
		ndmp_send_reply(session, &reply);
		return;
	}

	if (request->addr.addr_type == NDMP_ADDR_LOCAL) {
		/*
		 * Verify that the data server is listening for a
		 * local session.
		 */
		if (session->ns_data.dd_state != NDMP_DATA_STATE_LISTEN ||
		    session->ns_data.dd_listen_sock != -1) {
			ndmp_log(session, LOG_ERR,
			    "invalid data state for connect command");
			reply.error = NDMP_ILLEGAL_STATE_ERR;
		} else {
			session->ns_data.dd_state = NDMP_DATA_STATE_CONNECTED;
		}
	} else {
		assert(request->addr.addr_type == NDMP_ADDR_TCP);
		reply.error = mover_connect_sock_v3(session, request->mode,
		    request->addr.tcp_ip_v4(0), request->addr.tcp_port_v4(0));
	}

	if (reply.error == NDMP_NO_ERR) {
		session->ns_mover.md_data_addr.addr_type =
		    request->addr.addr_type;
		session->ns_mover.md_state = NDMP_MOVER_STATE_ACTIVE;
		session->ns_mover.md_mode = request->mode;
	}

	ndmp_send_reply(session, &reply);
}

/*
 * Write end-of-media magic string.  This is called after hitting the LEOT.
 */
void
ndmp_write_eom(ndmp_session_t *session, int fd)
{
	int n;

	(void) ndmp_mtioctl(session, fd, MTWEOF, 1);
	n = write(fd, NDMP_EOM_MAGIC, strlen(NDMP_EOM_MAGIC));

	ndmp_debug(session, "%d EOM bytes wrote", n);
	(void) ndmp_mtioctl(session, fd, MTWEOF, 1);

	/*
	 * Rewind to the previous file since the last two files are used
	 * as the indicator for logical EOM.
	 */
	(void) ndmp_mtioctl(session, fd, MTBSF, 2);
}

/*
 * Writes data to the remote mover.
 */
int
ndmp_remote_write(ndmp_session_t *session, char *data, ulong_t length)
{
	ssize_t n;
	ulong_t count = 0;

	while (count < length) {
		if (session->ns_eof)
			return (-1);

		if ((n = write(session->ns_data.dd_sock, &data[count],
		    length - count)) < 0) {
			session->ns_data.dd_errno = errno;
			ndmp_log(session, LOG_ERR,
			    "failed to write to socket %d: %s",
			    session->ns_data.dd_sock, strerror(errno));
			return (-1);
		}
		count += n;

		ndmp_debug(session, "wrote %ld bytes to "
		    "remote socket", n);
	}

	return (0);
}

/* *** ndmp internal functions ***************************************** */

/*
 * Initialize mover specific session variables.  Don't initialize variables
 * such as record_size that need to persist across data operations. A client
 * may open a session and do multiple backups after setting the record_size.
 */
int
ndmp_mover_init(ndmp_session_t *session)
{
	session->ns_mover.md_state = NDMP_MOVER_STATE_IDLE;
	session->ns_mover.md_pause_reason = NDMP_MOVER_PAUSE_NA;
	session->ns_mover.md_halt_reason = NDMP_MOVER_HALT_NA;
	session->ns_mover.md_data_written = 0LL;
	session->ns_mover.md_seek_position = 0LL;
	session->ns_mover.md_bytes_left_to_read = 0LL;
	session->ns_mover.md_window_offset = 0LL;
	session->ns_mover.md_window_length = MAX_WINDOW_SIZE;
	session->ns_mover.md_position = 0LL;
	session->ns_mover.md_discard_length = 0;
	session->ns_mover.md_record_num = 0;
	session->ns_mover.md_record_size = 0;
	session->ns_mover.md_listen_sock = -1;
	session->ns_mover.md_pre_cond = B_FALSE;
	session->ns_mover.md_sock = -1;
	session->ns_mover.md_r_index = 0;
	session->ns_mover.md_w_index = 0;
	session->ns_mover.md_buf = ndmp_malloc(session, MAX_RECORD_SIZE);
	if (session->ns_mover.md_buf == NULL)
		return (-1);

	if (session->ns_version == NDMPV3) {
		session->ns_mover.md_mode = NDMP_MOVER_MODE_READ;
		(void) memset(&session->ns_mover.md_data_addr, 0,
		    sizeof (ndmp_addr_v3));
	}
	return (0);
}

/*
 * Shutdown the mover. It closes all the sockets.
 */
void
ndmp_mover_shut_down(ndmp_session_t *session)
{
	if (session->ns_mover.md_listen_sock != -1) {
		ndmp_remove_file_handler(session,
		    session->ns_mover.md_listen_sock);
		(void) close(session->ns_mover.md_listen_sock);
		session->ns_mover.md_listen_sock = -1;
	}
	if (session->ns_mover.md_sock != -1) {
		ndmp_remove_file_handler(session,
		    session->ns_mover.md_sock);
		(void) close(session->ns_mover.md_sock);
		session->ns_mover.md_sock = -1;
	}
}

/*
 * Cleanup any mover data structures.
 */
void
ndmp_mover_cleanup(ndmp_session_t *session)
{
	free(session->ns_mover.md_buf);
	session->ns_mover.md_buf = NULL;
}

/*
 * Create a connection to the specified mover.
 */
ndmp_error
ndmp_mover_connect(ndmp_session_t *session, ndmp_mover_mode mover_mode)
{
	ndmp_mover_addr *mover = &session->ns_data.dd_mover;
	struct sockaddr_in sin;
	int sock = -1;
	int flag = 1;

	if (mover->addr_type == NDMP_ADDR_TCP) {
		if (mover->ndmp_mover_addr_u.addr.ip_addr) {
			(void) memset(&sin, 0, sizeof (sin));
			sin.sin_family = AF_INET;
			sin.sin_addr.s_addr =
			    mover->ndmp_mover_addr_u.addr.ip_addr;
			sin.sin_port =
			    mover->ndmp_mover_addr_u.addr.port;

			/*
			 * If the address type is TCP but both the address and
			 * the port number are zero, we have to use a different
			 * socket than the mover socket. This can happen when
			 * using NDMP disk to disk copy (AKA D2D copy).
			 * The NDMPCopy client will send a zero address to
			 * direct the server to use the mover socket as the
			 * data socket to receive the recovery data.
			 */
			if (sin.sin_addr.s_addr == 0 && sin.sin_port == 0) {
				ndmp_debug(session,
				    "setting data socket to mover socket");
				session->ns_data.dd_sock =
				    session->ns_mover.md_sock;
				return (NDMP_NO_ERR);
			}

			ndmp_debug(session, "addr: %u port: %u",
			    mover->ndmp_mover_addr_u.addr.ip_addr,
			    (ulong_t)sin.sin_port);

			if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
				ndmp_log(session, LOG_ERR,
				    "failed to open socket: %s",
				    strerror(errno));
				return (NDMP_IO_ERR);
			}
			if (connect(sock, (struct sockaddr *)&sin,
			    sizeof (sin)) < 0) {
				ndmp_log(session, LOG_ERR,
				    "failed to connect to remote host: %s",
				    strerror(errno));
				(void) close(sock);
				return (NDMP_IO_ERR);
			}

			if (ndmp_sbs > 0)
				ndmp_set_socket_snd_buf(session, sock,
				    ndmp_sbs * KILOBYTE);
			if (ndmp_rbs > 0)
				ndmp_set_socket_rcv_buf(session, sock,
				    ndmp_rbs * KILOBYTE);

			ndmp_set_socket_nodelay(sock);
			(void) setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &flag,
			    sizeof (flag));
		} else {
			if ((session->ns_mover.md_state !=
			    NDMP_MOVER_STATE_ACTIVE) ||
			    (session->ns_mover.md_sock == -1)) {

				ndmp_log(session, LOG_ERR,
				    "invalid mover state for connect request");
				return (NDMP_ILLEGAL_STATE_ERR);
			}

			sock = session->ns_mover.md_sock;
			ndmp_debug(session, "setting data sock fd: %d to be "
			    "same as listen_sock", sock);
		}

		session->ns_data.dd_sock = sock;

		ndmp_debug(session, "data sock: %u", sock);

		return (NDMP_NO_ERR);
	}

	/* Local mover session. */

	if (session->ns_mover.md_state != NDMP_MOVER_STATE_LISTEN) {
		ndmp_log(session, LOG_ERR,
		    "invalid mover state for connect request");
		return (NDMP_ILLEGAL_STATE_ERR);
	}
	if (session->ns_tape.td_fd == -1) {
		ndmp_log(session, LOG_ERR, "tape device not open");
		return (NDMP_DEV_NOT_OPEN_ERR);
	}
	if (mover_mode == NDMP_MOVER_MODE_READ &&
	    session->ns_tape.td_mode == NDMP_TAPE_READ_MODE) {
		ndmp_log(session, LOG_ERR, "write protected device");
		return (NDMP_WRITE_PROTECT_ERR);
	}

	session->ns_mover.md_state = NDMP_MOVER_STATE_ACTIVE;

	return (NDMP_NO_ERR);
}



/*
 * Seek to the requested data stream position.  If the requested offset is
 * outside of the current window, the mover is paused and a notify_mover_paused
 * request is sent notifying the client that a seek is required.  If the
 * requested offest is within the window but not within the current record,
 * then the tape is positioned to the record containing the requested offest.
 * The requested amount of data is then read from the tape device and written
 * to the data session.  Returns 0 on success, -1 on error, or 1 if there is
 * a seek pending completion by the NDMP client.
 */
int
ndmp_mover_seek(ndmp_session_t *session, u_longlong_t offset,
    u_longlong_t length)
{
	int ctlcmd;
	int ctlcnt;
	u_longlong_t tape_position;
	u_longlong_t buf_position;
	ndmp_notify_mover_paused_request pause_request;

	session->ns_mover.md_seek_position = offset;
	session->ns_mover.md_bytes_left_to_read = length;

	/*
	 * If the requested position is outside of the window,
	 * notify the client that a seek is required.
	 */
	if (session->ns_mover.md_seek_position <
	    session->ns_mover.md_window_offset ||
	    session->ns_mover.md_seek_position >=
	    session->ns_mover.md_window_offset +
	    session->ns_mover.md_window_length) {
		ndmp_debug(session, "MOVER_PAUSE_SEEK(%llu)",
		    session->ns_mover.md_seek_position);

		session->ns_mover.md_w_index = 0;
		session->ns_mover.md_r_index = 0;

		session->ns_mover.md_state = NDMP_MOVER_STATE_PAUSED;
		session->ns_mover.md_pause_reason = NDMP_MOVER_PAUSE_SEEK;
		pause_request.reason = NDMP_MOVER_PAUSE_SEEK;
		pause_request.seek_position = long_long_to_quad(offset);

		if (ndmp_send_request(session,
		    NDMP_NOTIFY_MOVER_PAUSED,
		    &pause_request, NULL) < 0) {
			return (-1);
		}
		return (1);
	}
	/*
	 * Determine the data stream position of the first byte in the
	 * data buffer.
	 */
	buf_position = session->ns_mover.md_position -
	    (session->ns_mover.md_position % session->ns_mover.md_record_size);

	/*
	 * Determine the data stream position of the next byte that
	 * will be read from tape.
	 */
	tape_position = buf_position;
	if (session->ns_mover.md_w_index != 0)
		tape_position += session->ns_mover.md_record_size;

	/*
	 * Check if requested position is for data that has been read and is
	 * in the buffer.
	 */
	if (offset >= buf_position && offset < tape_position) {
		session->ns_mover.md_position = offset;
		session->ns_mover.md_r_index = session->ns_mover.md_position -
		    buf_position;

		ndmp_debug(session, "seek pos %llu r_index %u",
		    session->ns_mover.md_position,
		    session->ns_mover.md_r_index);

		return (0);
	}

	ctlcmd = 0;
	if (tape_position > session->ns_mover.md_seek_position) {
		/* Need to seek backward. */
		ctlcmd = MTBSR;
		ctlcnt = (int)((tape_position - offset - 1)
		    / session->ns_mover.md_record_size) + 1;
		tape_position -= ((u_longlong_t)(((tape_position - offset - 1) /
		    session->ns_mover.md_record_size) + 1) *
		    (u_longlong_t)session->ns_mover.md_record_size);

	} else if (offset >= tape_position + session->ns_mover.md_record_size) {
		/* Need to seek forward. */
		ctlcmd = MTFSR;
		ctlcnt = (int)((offset - tape_position)
		    / session->ns_mover.md_record_size);
		tape_position += ((u_longlong_t)(((offset - tape_position) /
		    session->ns_mover.md_record_size)) *
		    (u_longlong_t)session->ns_mover.md_record_size);
	}

	/* Reposition the tape if necessary. */
	if (ctlcmd) {
		ndmp_debug(session, "seek cmd %d count %d", ctlcmd, ctlcnt);
		(void) ndmp_mtioctl(session, session->ns_tape.td_fd, ctlcmd,
		    ctlcnt);
	}

	session->ns_mover.md_position = tape_position;
	session->ns_mover.md_r_index = 0;
	session->ns_mover.md_w_index = 0;

	ndmp_debug(session, "seek pos %llu", session->ns_mover.md_position);

	return (0);
}

/*
 * Find out if the writer thread has started or not.
 */
static boolean_t
is_writer_running_v3(ndmp_session_t *session)
{
	return (session->ns_mover.md_data_addr.addr_type == NDMP_ADDR_TCP);
}

/*
 * Wait for the mover to enter the ACTIVE state.  This is called when we've
 * placed the mover into the PAUSED state.  We may or may not be in the main
 * processing thread, but either way we can grab the lock and safely process
 * any messages within this context.  It's theoretically possible that if this
 * function were called in main session handling context, we'd be blocking any
 * data operations from continuing, but in reality only the data threads can
 * end up waiting here, and they'd be unable to make forward progress since we
 * must be waiting for data to become available.
 */
int
ndmp_mover_wait(ndmp_session_t *session)
{
	(void) mutex_lock(&session->ns_lock);
	while (session->ns_mover.md_state != NDMP_MOVER_STATE_ACTIVE) {
		if (ndmp_process_requests(session, B_TRUE) != 0) {
			(void) mutex_unlock(&session->ns_lock);
			return (-1);
		}
	}
	(void) mutex_unlock(&session->ns_lock);

	return (0);
}

/*
 * This function sends the notify message to the client.
 */
int
ndmp_mover_error_send(ndmp_session_t *session, ndmp_mover_halt_reason reason)
{
	ndmp_notify_mover_halted_request req;

	req.reason = reason;
	req.text_reason = "";

	return (ndmp_send_request(session,
	    NDMP_NOTIFY_MOVER_HALTED, &req, NULL));
}

/*
 * This function sends the notify message to the client.
 */
int
ndmp_mover_error_send_v4(ndmp_session_t *session,
    ndmp_mover_halt_reason reason)
{
	ndmp_notify_mover_halted_request_v4 req;

	req.reason = reason;

	return (ndmp_send_request(session,
	    NDMP_NOTIFY_MOVER_HALTED, &req, NULL));
}

/*
 * This function is called when an unrecoverable mover error has been detected.
 * A notify message is sent to the client and the mover is placed into the
 * halted state.
 */
void
ndmp_mover_error(ndmp_session_t *session, ndmp_mover_halt_reason reason)
{
	if (session->ns_mover.md_state == NDMP_MOVER_STATE_HALTED ||
	    session->ns_mover.md_state == NDMP_MOVER_STATE_IDLE)
		return;

	if (session->ns_version == NDMPV4) {
		(void) ndmp_mover_error_send_v4(session, reason);
	} else {
		/* No media error in V3 */
		if (reason == NDMP_MOVER_HALT_MEDIA_ERROR)
			reason = NDMP_MOVER_HALT_INTERNAL_ERROR;
		(void) ndmp_mover_error_send(session, reason);
	}

	if (session->ns_mover.md_listen_sock != -1) {
		ndmp_remove_file_handler(session,
		    session->ns_mover.md_listen_sock);
		(void) close(session->ns_mover.md_listen_sock);
		session->ns_mover.md_listen_sock = -1;
	}
	if (session->ns_mover.md_sock != -1) {
		ndmp_remove_file_handler(session,
		    session->ns_mover.md_sock);
		(void) close(session->ns_mover.md_sock);
		session->ns_mover.md_sock = -1;
	}

	session->ns_mover.md_state = NDMP_MOVER_STATE_HALTED;
	session->ns_mover.md_halt_reason = reason;
}

/*
 * Send an ndmp_notify_mover_paused request to the NDMP client to inform the
 * client that its attention is required.  Process messages until the
 * data/mover operation is either aborted or continued.
 */
static int
mover_pause_v3(ndmp_session_t *session, ndmp_mover_pause_reason reason)
{
	int rv;
	ndmp_notify_mover_paused_request request;

	rv = 0;
	session->ns_mover.md_state = NDMP_MOVER_STATE_PAUSED;
	session->ns_mover.md_pause_reason = reason;
	session->ns_mover.md_pre_cond = B_FALSE;

	request.reason = session->ns_mover.md_pause_reason;
	request.seek_position =
	    long_long_to_quad(session->ns_mover.md_position);

	if (ndmp_send_request(session, NDMP_NOTIFY_MOVER_PAUSED,
	    &request, NULL) < 0) {
		return (-1);
	}

	rv = ndmp_mover_wait(session);
	if (rv == 0)
		session->ns_tape.td_record_count = 0;

	return (rv);
}

/*
 * Writes a data record to tape. Detects and handles EOT conditions.  Returns
 * the number of bytes written, -1 on error, or 0 if the operation was aborted
 * by the client.
 */
static int
mover_tape_write_v3(ndmp_session_t *session, char *data, ssize_t length)
{
	ssize_t n;
	int err;

	for (;;) {
		/*
		 * Refer to the comment at the top of ndmp_tape.c file for
		 * Mammoth2 tape drives.
		 */
		if (session->ns_tape.td_eom_seen) {
			ndmp_debug(session, "eom_seen");

			session->ns_tape.td_eom_seen = B_FALSE;
			/*
			 * End of media reached.
			 * Notify client and wait for the client to
			 * either abort the operation or continue the
			 * operation after changing the tape.
			 */
			ndmp_log(session, LOG_INFO,
			    "End of tape reached. Load next tape.");

			err = mover_pause_v3(session, NDMP_MOVER_PAUSE_EOM);

			/* Operation aborted or connection terminated? */
			if (err < 0)
				return (-1);

			/* Retry the write to the new tape. */
			continue;
		}

		/*
		 * Enforce mover window on write.
		 */
		if (session->ns_mover.md_position >=
		    session->ns_mover.md_window_offset +
		    session->ns_mover.md_window_length) {
			ndmp_debug(session, "MOVER_PAUSE_EOW, "
			    "position = %llu, window = [%llu, %llu]",
			    session->ns_mover.md_position,
			    session->ns_mover.md_window_offset,
			    session->ns_mover.md_window_length);

			err = mover_pause_v3(session, NDMP_MOVER_PAUSE_EOW);
			/* Operation aborted or connection terminated? */
			if (err < 0)
				return (-1);

		}

		n = write(session->ns_tape.td_fd, data, length);
		if (n < 0) {
			ndmp_log(session, LOG_ERR,
			    "tape write error: %s", strerror(errno));
			return (-1);
		}

		if (n == 0 || n != length) {
			if (n != 0) {
				/*
				 * Backup one record since the record
				 * hits the EOM.
				 */
				ndmp_debug(session, "Back up one record");
				(void) ndmp_mtioctl(session,
				    session->ns_tape.td_fd, MTBSR, 1);

				/* setting logical EOM */
				ndmp_write_eom(session,
				    session->ns_tape.td_fd);
			}

			/*
			 * End of media reached.
			 * Notify client and wait for the client to
			 * either abort the operation or continue the
			 * operation after changing the tape.
			 */
			ndmp_log(session, LOG_INFO,
			    "End of tape reached. Load next tape.");

			err = mover_pause_v3(session, NDMP_MOVER_PAUSE_EOM);

			/* Operation aborted or connection terminated? */
			if (err < 0)
				return (-1);

			/* Retry the write to the new tape. */
			continue;
		}

		session->ns_tape.td_record_count++;
		return (n);
	}
}

/*
 * Writes all remaining buffered data to tape. A partial record is
 * padded out to a full record with zeros.
 */
static int
mover_tape_flush_v3(ndmp_session_t *session)
{
	int n;

	if (session->ns_mover.md_w_index == 0)
		return (0);

	(void) memset((void*)&session->ns_mover.md_buf[session->
	    ns_mover.md_w_index], 0,
	    session->ns_mover.md_record_size - session->ns_mover.md_w_index);

	n = mover_tape_write_v3(session, session->ns_mover.md_buf,
	    session->ns_mover.md_record_size);
	if (n < 0) {
		ndmp_log(session, LOG_ERR, "tape write error: %s",
		    strerror(errno));
		return (-1);
	}

	session->ns_mover.md_w_index = 0;
	session->ns_mover.md_position += n;
	return (n);
}

/*
 * Buffers and writes data to the tape device.  A full tape record is buffered
 * before being written.
 */
int
ndmp_local_write_v3(ndmp_session_t *session, char *data, ulong_t length)
{
	ulong_t count = 0;
	ssize_t n;
	ulong_t len;

	if (session->ns_mover.md_state == NDMP_MOVER_STATE_IDLE ||
	    session->ns_mover.md_state == NDMP_MOVER_STATE_LISTEN ||
	    session->ns_mover.md_state == NDMP_MOVER_STATE_HALTED) {
		ndmp_log(session, LOG_ERR, "invalid mover state to write data");
		return (-1);
	}

	/*
	 * A length of 0 indicates that any buffered data should be
	 * flushed to tape.
	 */
	if (length == 0) {
		if (session->ns_mover.md_w_index == 0)
			return (0);

		(void) memset((void*)&session->ns_mover.md_buf[session->
		    ns_mover.md_w_index], 0, session->ns_mover.md_record_size -
		    session->ns_mover.md_w_index);

		n = mover_tape_write_v3(session, session->ns_mover.md_buf,
		    session->ns_mover.md_record_size);
		if (n <= 0) {
			ndmp_mover_error(session,
			    (n == 0 ?  NDMP_MOVER_HALT_ABORTED :
			    NDMP_MOVER_HALT_MEDIA_ERROR));
			return (-1);
		}

		session->ns_mover.md_position += n;
		session->ns_mover.md_data_written +=
		    session->ns_mover.md_w_index;
		session->ns_mover.md_record_num++;
		session->ns_mover.md_w_index = 0;
		return (0);
	}

	/* Break the data into records. */
	while (count < length) {
		/*
		 * Determine if data needs to be buffered or can be written
		 * directly from user supplied location.  We can fast path the
		 * write if there is no pending buffered data and there is at
		 * least a full records worth of data to be written.
		 */
		if (session->ns_mover.md_w_index == 0 &&
		    length - count >= session->ns_mover.md_record_size) {
			n = mover_tape_write_v3(session, &data[count],
			    session->ns_mover.md_record_size);
			if (n <= 0) {
				ndmp_mover_error(session,
				    (n == 0 ?  NDMP_MOVER_HALT_ABORTED :
				    NDMP_MOVER_HALT_MEDIA_ERROR));
				return (-1);
			}

			session->ns_mover.md_position += n;
			session->ns_mover.md_data_written += n;
			session->ns_mover.md_record_num++;
			count += n;
			continue;
		}

		/* Buffer the data */
		len = length - count;
		if (len > session->ns_mover.md_record_size -
		    session->ns_mover.md_w_index)
			len = session->ns_mover.md_record_size -
			    session->ns_mover.md_w_index;

		(void) memcpy(&session->ns_mover.md_buf[session->
		    ns_mover.md_w_index], &data[count], len);
		session->ns_mover.md_w_index += len;
		count += len;

		/* Write the buffer if its full */
		if (session->ns_mover.md_w_index ==
		    session->ns_mover.md_record_size) {
			n = mover_tape_write_v3(session,
			    session->ns_mover.md_buf,
			    session->ns_mover.md_record_size);
			if (n < 0) {
				ndmp_mover_error(session,
				    (n == 0 ?  NDMP_MOVER_HALT_ABORTED :
				    NDMP_MOVER_HALT_MEDIA_ERROR));
				return (-1);
			}

			session->ns_mover.md_position += n;
			session->ns_mover.md_data_written += n;
			session->ns_mover.md_record_num++;
			session->ns_mover.md_w_index = 0;
		}
	}

	return (0);
}

/*
 * Reads backup data from the data connection and writes the received data to
 * the tape device.
 */
/*ARGSUSED*/
static void
mover_data_read_v3(ndmp_session_t *session, int fd, ulong_t mode)
{
	int n;
	ulong_t index;

	n = read(fd, &session->ns_mover.md_buf[session->ns_mover.md_w_index],
	    session->ns_mover.md_record_size - session->ns_mover.md_w_index);

	ndmp_debug(session, "mover read = %d\n", n);

	/*
	 * Since this function is only called when select believes data
	 * is available to be read, a return of zero indicates the
	 * connection has been closed.
	 */
	if (n <= 0) {
		ndmp_debug(session, "read() errno = %d\n", errno);
		if (n < 0 && errno == EWOULDBLOCK)
			return;

		/* Save the index since mover_tape_flush_v3 resets it. */
		index = session->ns_mover.md_w_index;

		/* Flush any buffered data to tape. */
		if (mover_tape_flush_v3(session) > 0) {
			session->ns_mover.md_data_written += index;
			session->ns_mover.md_record_num++;
		}

		if (n == 0)
			ndmp_mover_error(session,
			    NDMP_MOVER_HALT_CONNECT_CLOSED);
		else
			ndmp_mover_error(session,
			    NDMP_MOVER_HALT_INTERNAL_ERROR);

		return;
	}

	session->ns_mover.md_w_index += n;

	if (session->ns_mover.md_w_index == session->ns_mover.md_record_size) {
		n = mover_tape_write_v3(session, session->ns_mover.md_buf,
		    session->ns_mover.md_record_size);
		if (n <= 0) {
			ndmp_mover_error(session,
			    (n == 0 ? NDMP_MOVER_HALT_ABORTED :
			    NDMP_MOVER_HALT_MEDIA_ERROR));
			return;
		}

		session->ns_mover.md_position += n;
		session->ns_mover.md_w_index = 0;
		session->ns_mover.md_data_written += n;
		session->ns_mover.md_record_num++;
	}
}

/*
 * Reads a data record from tape. Detects and handles EOT conditions.
 *
 * Returns:
 *   0			operation aborted.
 *   TAPE_READ_ERR	tape read IO error.
 *   TAPE_NO_WRITER_ERR	no writer is running during tape read
 * 			otherwise - number of bytes read.
 */
static int
mover_tape_read_v3(ndmp_session_t *session, char *data)
{
	ssize_t	 n;
	int err;
	int count;

	count = session->ns_mover.md_record_size;
	for (; ; ) {
		n = read(session->ns_tape.td_fd, data, count);
		if (n < 0) {
			ndmp_log(session, LOG_ERR, "tape read error: %s",
			    strerror(errno));
			return (TAPE_READ_ERR);
		}

		if (n == 0) {
			if (!is_writer_running_v3(session))
				return (TAPE_NO_WRITER_ERR);

			/*
			 * End of media reached.
			 * Notify client and wait for the client to
			 * either abort the data operation or continue the
			 * operation after changing the tape.
			 */
			ndmp_log(session, LOG_INFO,
			    "End of tape reached. Load next tape.");

			err = mover_pause_v3(session, NDMP_MOVER_PAUSE_EOF);

			/* Operation aborted or connection terminated? */
			if (err < 0) {
				/*
				 * Back up one record if it's read but not
				 * used.
				 */
				if (count != session->ns_mover.md_record_size)
					(void) ndmp_mtioctl(session,
					    session->ns_tape.td_fd, MTBSR, 1);
				return (0);
			}

			/* Retry the read from the new tape. */
			continue;
		}

		data += n;
		count -= n;
		if (count <= 0) {
			session->ns_mover.md_record_num++;
			session->ns_tape.td_record_count++;
			return (n);
		}
	}
}


/*
 * Reads backup data from the tape device and writes the data to the data
 * session.  This function is called by ndmp_select when the data
 * connection is ready for more data to be written.
 */
/*ARGSUSED*/
static void
mover_data_write_v3(ndmp_session_t *session, int fd, ulong_t mode)
{
	int n;
	ulong_t len;
	u_longlong_t wlen;
	ndmp_notify_mover_paused_request pause_request;

	/*
	 * If the end of the mover window has been reached,
	 * then notify the client that a seek is needed.
	 * Remove the file handler to prevent this function from
	 * being called. The handler will be reinstalled in
	 * ndmp_mover_continue.
	 */
	if (session->ns_mover.md_position >= session->ns_mover.md_window_offset
	    + session->ns_mover.md_window_length) {
		ndmp_debug(session, "MOVER_PAUSE_SEEK(%llu)",
		    session->ns_mover.md_position);

		session->ns_mover.md_w_index = 0;
		session->ns_mover.md_r_index = 0;

		session->ns_mover.md_state = NDMP_MOVER_STATE_PAUSED;
		session->ns_mover.md_pause_reason = NDMP_MOVER_PAUSE_SEEK;
		pause_request.reason = NDMP_MOVER_PAUSE_SEEK;
		pause_request.seek_position =
		    long_long_to_quad(session->ns_mover.md_position);
		session->ns_mover.md_seek_position =
		    session->ns_mover.md_position;

		ndmp_remove_file_handler(session, fd);

		if (ndmp_send_request(session,
		    NDMP_NOTIFY_MOVER_PAUSED,
		    &pause_request, NULL) < 0) {
			ndmp_mover_error(session,
			    NDMP_MOVER_HALT_INTERNAL_ERROR);
		}
		return;
	}

	/*
	 * Read more data into the tape buffer if the buffer is empty.
	 */
	if (session->ns_mover.md_w_index == 0) {
		n = mover_tape_read_v3(session, session->ns_mover.md_buf);

		ndmp_debug(session, "read %u bytes from tape", n);

		if (n <= 0) {
			ndmp_mover_error(session, (n == 0 ?
			    NDMP_MOVER_HALT_ABORTED
			    : NDMP_MOVER_HALT_MEDIA_ERROR));
			return;
		}

		/*
		 * Discard data if the current data stream position is
		 * prior to the seek position. This is necessary if a seek
		 * request set the seek pointer to a position that is not a
		 * record boundary. The seek request handler can only position
		 * to the start of a record.
		 */
		if (session->ns_mover.md_position <
		    session->ns_mover.md_seek_position) {
			session->ns_mover.md_r_index =
			    session->ns_mover.md_seek_position -
			    session->ns_mover.md_position;
			session->ns_mover.md_position =
			    session->ns_mover.md_seek_position;
		}

		session->ns_mover.md_w_index = n;
	}

	/*
	 * The limit on the total amount of data to be sent can be dictated by
	 * either the end of the mover window or the end of the seek window.
	 * First determine which window applies and then determine if the send
	 * length needs to be less than a full record to avoid exceeding the
	 * window.
	 */
	if (session->ns_mover.md_position +
	    session->ns_mover.md_bytes_left_to_read >
	    session->ns_mover.md_window_offset +
	    session->ns_mover.md_window_length)
		wlen = session->ns_mover.md_window_offset +
		    session->ns_mover.md_window_length -
		    session->ns_mover.md_position;
	else
		wlen = session->ns_mover.md_bytes_left_to_read;

	/*
	 * Now limit the length to the amount of data in the buffer.
	 */
	if (wlen > session->ns_mover.md_w_index - session->ns_mover.md_r_index)
		wlen = session->ns_mover.md_w_index -
		    session->ns_mover.md_r_index;

	len = wlen & 0xffffffff;

	/*
	 * Write the data to the data session.
	 */
	n = write(session->ns_mover.md_sock,
	    &session->ns_mover.md_buf[session->ns_mover.md_r_index], len);

	if (n < 0) {
		if (errno == EWOULDBLOCK) {
			ndmp_debug(session, "n %d errno %d", n, errno);
			return;
		}

		ndmp_debug(session, "n %d errno %d", n, errno);
		ndmp_mover_error(session, NDMP_MOVER_HALT_CONNECT_CLOSED);
		return;
	}

	ndmp_debug(session, "wrote %u of %u bytes to data connection "
	    "position %llu r_index %lu",
	    n, len, session->ns_mover.md_position,
	    session->ns_mover.md_r_index);

	session->ns_mover.md_r_index += n;
	session->ns_mover.md_position += n;
	session->ns_mover.md_bytes_left_to_read -= n;

	/*
	 * If all data in the buffer has been written, zero the buffer indices.
	 * The next call to this function will read more data from the tape
	 * device into the buffer.
	 */
	if (session->ns_mover.md_r_index == session->ns_mover.md_w_index) {
		session->ns_mover.md_r_index = 0;
		session->ns_mover.md_w_index = 0;
	}

	/*
	 * If the read limit has been reached, then remove the file handler to
	 * prevent this function from getting called. The next mover_read
	 * request will reinstall the handler.
	 */
	if (session->ns_mover.md_bytes_left_to_read == 0)
		ndmp_remove_file_handler(session, fd);
}

/*
 * Accept a data connection from a data server.  Called by ndmp_select when a
 * connection is pending on the mover listen socket.
 */
/*ARGSUSED*/
static void
accept_connection_v3(ndmp_session_t *session, int fd, ulong_t mode)
{
	int from_len;
	struct sockaddr_in from;
	int flag = 1;

	from_len = sizeof (from);
	session->ns_mover.md_sock = accept(fd, (struct sockaddr *)&from,
	    &from_len);

	ndmp_debug(session, "sin: port %d addr %s", ntohs(from.sin_port),
	    inet_ntoa(IN_ADDR(from.sin_addr.s_addr)));

	ndmp_remove_file_handler(session, fd);
	(void) close(session->ns_mover.md_listen_sock);
	session->ns_mover.md_listen_sock = -1;

	if (session->ns_mover.md_sock < 0) {
		ndmp_log(session, LOG_ERR, "failed to accept session: %s",
		    strerror(errno));
		ndmp_mover_error(session, NDMP_MOVER_HALT_CONNECT_ERROR);
		return;
	}

	/*
	 * Save the peer address.
	 */
	session->ns_mover.md_data_addr.tcp_ip_v3 = from.sin_addr.s_addr;
	session->ns_mover.md_data_addr.tcp_port_v3 = from.sin_port;

	/*
	 * Set the parameter of the new socket.
	 */
	(void) setsockopt(session->ns_mover.md_sock, SOL_SOCKET, SO_KEEPALIVE,
	    &flag, sizeof (flag));

	ndmp_set_socket_nodelay(session->ns_mover.md_sock);
	if (ndmp_sbs > 0)
		ndmp_set_socket_snd_buf(session, session->ns_mover.md_sock,
		    ndmp_sbs*KILOBYTE);
	if (ndmp_rbs > 0)
		ndmp_set_socket_rcv_buf(session, session->ns_mover.md_sock,
		    ndmp_rbs*KILOBYTE);

	ndmp_debug(session, "sock fd: %d", session->ns_mover.md_sock);

	if (session->ns_mover.md_mode == NDMP_MOVER_MODE_READ) {
		if (ndmp_add_file_handler(session, (void*)session,
		    session->ns_mover.md_sock, NDMPD_SELECT_MODE_READ,
		    HC_MOVER, mover_data_read_v3) < 0) {
			ndmp_mover_error(session,
			    NDMP_MOVER_HALT_INTERNAL_ERROR);
			return;
		}
		ndmp_debug(session, "Backup connection established by %s:%d",
		    inet_ntoa(IN_ADDR(from.sin_addr.s_addr)),
		    ntohs(from.sin_port));
	} else {
		ndmp_debug(session, "Restore connection established by %s:%d",
		    inet_ntoa(IN_ADDR(from.sin_addr.s_addr)),
		    ntohs(from.sin_port));
	}

	session->ns_mover.md_state = NDMP_MOVER_STATE_ACTIVE;
}

/*
 * Creates a socket for listening for accepting data sessions.
 */
static int
create_listen_socket_v3(ndmp_session_t *session, ulong_t *addr, ushort_t *port)
{
	session->ns_mover.md_listen_sock = ndmp_create_socket(session, addr,
	    port);
	if (session->ns_mover.md_listen_sock < 0)
		return (-1);

	/*
	 * Add a file handler for the listen socket.  ndmp_select will call
	 * accept_session_v3 when a connection is ready to be accepted.
	 */
	if (ndmp_add_file_handler(session, session,
	    session->ns_mover.md_listen_sock, NDMPD_SELECT_MODE_READ, HC_MOVER,
	    accept_connection_v3) < 0) {
		(void) close(session->ns_mover.md_listen_sock);
		session->ns_mover.md_listen_sock = -1;
		return (-1);
	}
	ndmp_debug(session, "IP %s port %d",
	    inet_ntoa(*(struct in_addr *)addr), ntohs(*port));
	return (0);
}

/*
 * Connect the mover to the specified address
 */
static ndmp_error
mover_connect_sock_v3(ndmp_session_t *session, ndmp_mover_mode mode,
    ulong_t addr, ushort_t port)
{
	int sock;

	sock = ndmp_connect_sock_v3(session, addr, port);
	if (sock < 0)
		return (NDMP_CONNECT_ERR);

	if (mode == NDMP_MOVER_MODE_READ) {
		if (ndmp_add_file_handler(session, session, sock,
		    NDMPD_SELECT_MODE_READ, HC_MOVER, mover_data_read_v3) < 0) {
			(void) close(sock);
			return (NDMP_CONNECT_ERR);
		}
	}
	session->ns_mover.md_sock = sock;
	session->ns_mover.md_data_addr.addr_type = NDMP_ADDR_TCP;
	session->ns_mover.md_data_addr.tcp_ip_v3 = addr;
	session->ns_mover.md_data_addr.tcp_port_v3 = port;
	return (NDMP_NO_ERR);
}

/*
 * Reads data from the local tape device.  Full tape records are read and
 * buffered.  Returns 0 on success, -1 on error, or 1 if the writer is not
 * running.
 */
int
ndmp_local_read_v3(ndmp_session_t *session, char *data, ulong_t length)
{
	ulong_t count;
	ulong_t len;
	ssize_t n;

	count = 0;
	if (session->ns_mover.md_state == NDMP_MOVER_STATE_IDLE ||
	    session->ns_mover.md_state == NDMP_MOVER_STATE_LISTEN ||
	    session->ns_mover.md_state == NDMP_MOVER_STATE_HALTED) {
		ndmp_log(session, LOG_ERR, "invalid mover state to read data",
		    strerror(errno));
		return (-1);
	}

	/*
	 * Automatically increase the seek window if necessary.
	 * This is needed in the event the module attempts to read
	 * past a seek window set via a prior call to ndmp_seek() or
	 * the module has not issued a seek.
	 */
	if (length > session->ns_mover.md_bytes_left_to_read) {
		/*
		 * If no seek was issued then pretend that a seek was issued to
		 * read the entire tape.
		 */
		if (session->ns_data.dd_read_length == 0) {
			session->ns_mover.md_bytes_left_to_read = ~0LL;
			session->ns_data.dd_read_offset = 0LL;
			session->ns_data.dd_read_length = ~0LL;
		} else {
			session->ns_mover.md_bytes_left_to_read = length;
			session->ns_data.dd_read_offset =
			    session->ns_mover.md_position;
			session->ns_data.dd_read_length = length;
		}
	}

	/*
	 * Read as many records as necessary to satisfy the request.
	 */
	while (count < length) {
		/*
		 * If the end of the mover window has been reached,
		 * then notify the client that a new data window is needed.
		 */
		if (session->ns_mover.md_position >=
		    session->ns_mover.md_window_offset +
		    session->ns_mover.md_window_length) {
			if (mover_pause_v3(session,
			    NDMP_MOVER_PAUSE_SEEK) < 0) {
				ndmp_mover_error(session,
				    NDMP_MOVER_HALT_INTERNAL_ERROR);
				return (-1);
			}
			continue;
		}

		len = length - count;

		/*
		 * Prevent reading past the end of the window.
		 */
		if (len > session->ns_mover.md_window_offset +
		    session->ns_mover.md_window_length -
		    session->ns_mover.md_position)
			len = session->ns_mover.md_window_offset +
			    session->ns_mover.md_window_length -
			    session->ns_mover.md_position;

		/*
		 * Copy from the data buffer first.
		 */
		if (session->ns_mover.md_w_index -
		    session->ns_mover.md_r_index != 0) {
			/*
			 * Limit the copy to the amount of data in the buffer.
			 */
			if (len > session->ns_mover.md_w_index -
			    session->ns_mover.md_r_index)
				len = session->ns_mover.md_w_index -
				    session->ns_mover.md_r_index;
			(void) memcpy((void*)&data[count],
			    &session->ns_mover.md_buf[session->
			    ns_mover.md_r_index], len);
			count += len;
			session->ns_mover.md_r_index += len;
			session->ns_mover.md_bytes_left_to_read -= len;
			session->ns_mover.md_position += len;
			continue;
		}

		/*
		 * Determine if data needs to be buffered or
		 * can be read directly to user supplied location.
		 * We can fast path the read if at least a full record
		 * needs to be read and there is no seek pending.
		 * This is done to eliminate a buffer copy.
		 */
		if (len >= session->ns_mover.md_record_size &&
		    session->ns_mover.md_position >=
		    session->ns_mover.md_seek_position) {
			n = mover_tape_read_v3(session, &data[count]);
			if (n <= 0) {
				if (n == TAPE_NO_WRITER_ERR)
					return (1);

				ndmp_mover_error(session,
				    (n == 0 ? NDMP_MOVER_HALT_ABORTED :
				    NDMP_MOVER_HALT_MEDIA_ERROR));
				return ((n == 0) ? 1 : -1);
			}

			count += n;
			session->ns_mover.md_bytes_left_to_read -= n;
			session->ns_mover.md_position += n;
			continue;
		}

		/* Read the next record into the buffer. */
		n = mover_tape_read_v3(session, session->ns_mover.md_buf);
		if (n <= 0) {
			if (n == TAPE_NO_WRITER_ERR)
				return (1);

			ndmp_mover_error(session,
			    (n == 0 ? NDMP_MOVER_HALT_ABORTED :
			    NDMP_MOVER_HALT_MEDIA_ERROR));
			return ((n == 0) ? 1 : -1);
		}

		session->ns_mover.md_w_index = n;
		session->ns_mover.md_r_index = 0;

		/*
		 * Discard data if the current data stream position is
		 * prior to the seek position. This is necessary if a seek
		 * request set the seek pointer to a position that is not a
		 * record boundary. The seek request handler can only position
		 * to the start of a record.
		 */
		if (session->ns_mover.md_position <
		    session->ns_mover.md_seek_position) {
			session->ns_mover.md_r_index =
			    session->ns_mover.md_seek_position -
			    session->ns_mover.md_position;
			session->ns_mover.md_position =
			    session->ns_mover.md_seek_position;
		}
	}

	return (0);
}
