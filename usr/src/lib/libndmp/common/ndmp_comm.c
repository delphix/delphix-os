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
 * Main communication functions.  This file contains functions related to
 * creating and managing the main NDMP control sockets, as well as message
 * processing.  Messages are defined in ndmp.h, with ndmp_handler.c describing
 * how we encode, decode, and process messages and replies.
 */

typedef enum {
	NDMP_PROC_ERR,		/* session error */
	NDMP_PROC_MSG,		/* reply available */
	NDMP_PROC_REP,		/* no reply */
	NDMP_PROC_REP_ERR,	/* error processing reply */
} ndmp_proc_ret_t;

static ndmp_proc_ret_t ndmp_process_messages(ndmp_session_t *, boolean_t,
    boolean_t);
static ndmp_handler_t *ndmp_get_interface(ndmp_message message);

/*
 * Return the NDMP interface handler (e.g. config, scsi, tape) for the specific
 * message.
 */
static ndmp_handler_t *
ndmp_get_interface(ndmp_message message)
{
	uint_t class = (message >> 8);
	ndmp_handler_t *ni;

	if (class >= INT_MAXCLASS)
		return (NULL);

	ni = &ndmp_msghdl_tab[class];
	if ((message & 0xff) >= ni->hd_cnt)
		return (NULL);

	/* Sanity check */
	if (ni->hd_msgs[message & 0xff].hm_message != message)
		return (NULL);

	return (ni);
}

/*
 * Return the message handler info for the specified NDMP message.  This
 * enforces client- or server-wide restrictions in the process.  If a request
 * is received that is invalid in the current context, we return NULL even
 * though we have a handler available.
 */
static ndmp_msg_handler_t *
ndmp_get_handler(ndmp_session_t *session, ndmp_message message,
    ndmp_header_message_type type, boolean_t isreq, const char **messagestr)
{
	uint_t class = (message >> 8);
	ndmp_msg_handler_t *handler = NULL;
	int ver = session->ns_version;
	ndmp_handler_t *ni;

	if (class >= INT_MAXCLASS)
		return (NULL);

	ni = ndmp_get_interface(message);

	if (ni == NULL)
		return (NULL);

	/*
	 * We filter what requests we allow based on client and server settings.
	 */
	if (isreq && type == NDMP_MESSAGE_REQUEST) {
		if (session->ns_server != NULL) {
			/* Ignore client-side requests if in server mode */
			if (class == NDMP_MESSAGE_LOG ||
			    class == NDMP_MESSAGE_FH) {
				return (NULL);
			}

			/* Ignore SCSI and TAPE requests if requested */
			if (!ndmp_get_prop_boolean(session, NDMP_LOCAL_TAPE) &&
			    (class == NDMP_MESSAGE_SCSI ||
			    class == NDMP_MESSAGE_TAPE)) {
				return (NULL);
			}
		} else {
			/* Ignore server-side requests if in client mode */
			if (class != NDMP_MESSAGE_LOG &&
			    class != NDMP_MESSAGE_FH &&
			    class != NDMP_MESSAGE_NOTIFY) {
				return (NULL);
			}
		}
	}

	handler = &ni->hd_msgs[message & 0xff].hm_msg_v[ver - NDMPV3];
	*messagestr = ni->hd_msgs[message & 0xff].hm_messagestr;

	return (handler);
}

/*
 * Check if the session needs to be authenticated before this message is
 * processed.
 */
static boolean_t
ndmp_check_auth_required(ndmp_message message)
{
	boolean_t auth_req = B_FALSE;
	ndmp_handler_t *ni = ndmp_get_interface(message);

	if (ni != NULL)
		auth_req = ni->hd_msgs[message & 0xff].hm_auth_required;

	return (auth_req);
}

/*
 * This is the main server listener thread.  This thread accepts new connection
 * requests and creates a server session for each one.
 */
void *
ndmp_server_run(void *param)
{
	ndmp_server_t *server = param;
	ndmp_session_t *session = &server->ns_global_session;
	int ns;
	struct sockaddr_in sin;
	int tmp, len, flag = 1;

	for (;;) {
		len = sizeof (sin);
		if ((ns = accept(server->ns_listen_socket,
		    (struct sockaddr *)&sin, &len)) < 0) {
			/*
			 * Main listener socket was closed by
			 * ndmp_server_destroy(), exit.
			 */
			if (server->ns_shutdown)
				break;

			ndmp_log(session, LOG_ERR,
			    "failed to accept socket: %s", strerror(errno));
			continue;
		}

		/*
		 * 'css' and 'crs' in the following env variables stand for:
		 * 'session send size' and 'session receive size'.
		 */
		tmp = ndmp_get_prop_int(session, NDMP_SOCKET_CSS);
		if (tmp <= 0)
			tmp = 65;
		ndmp_set_socket_snd_buf(session, ns, tmp * KILOBYTE);

		tmp = ndmp_get_prop_int(session, NDMP_SOCKET_CRS);
		if (tmp <= 0)
			tmp = 80;
		ndmp_set_socket_rcv_buf(session, ns, tmp * KILOBYTE);

		ndmp_set_socket_nodelay(ns);
		(void) setsockopt(ns, SOL_SOCKET, SO_KEEPALIVE, &flag,
		    sizeof (flag));

		(void) ndmp_session_create(session, ns);
	}

	ndmp_log(session, LOG_INFO, "NDMP server shutting down");

	return (0);
}

/*
 * Creates a client session connected to a remote server.  If 'port' is 0 it
 * will use the default port.
 */
ndmp_session_t *
ndmp_connect(ndmp_client_t *client, const char *host, int port)
{
	ndmp_session_t *global_session = &client->nc_global_session;
	struct addrinfo req = { 0 };
	struct addrinfo *res;
	int sock;
	char *portbuf;

	if (port == 0)
		port = ndmp_get_prop_int(global_session, NDMP_TCP_PORT);

	NDMP_ASPRINTF(&portbuf, "%d", port);

	req.ai_family = PF_INET;
	req.ai_socktype = SOCK_STREAM;
	req.ai_flags = AI_NUMERICSERV;

	if (getaddrinfo(host, portbuf, &req, &res) != 0) {
		ndmp_log(global_session, LOG_ERR, "unable to resolve host '%s'",
		    host);
		return (NULL);
	}

	sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sock < 0) {
		ndmp_log(global_session, LOG_ERR, "failed to create socket: %s",
		    strerror(errno));
		freeaddrinfo(res);
		return (NULL);
	}

	if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
		ndmp_log(global_session, LOG_ERR,
		    "failed to connect to host: %s", strerror(errno));
		(void) close(sock);
		freeaddrinfo(res);
		return (NULL);
	}

	freeaddrinfo(res);

	return (ndmp_session_create(global_session, sock));
}

/*
 * Process requests until there is no more data to read.  Invoked by the
 * session handler function.
 */
int
ndmp_process_requests(ndmp_session_t *session, boolean_t stop_on_abort)
{
	int rv;

	(void) mutex_lock(&session->ns_lock);
	rv = 0;
	if (ndmp_process_messages(session, B_FALSE,
	    stop_on_abort) == NDMP_PROC_ERR)
		rv = -1;
	(void) mutex_unlock(&session->ns_lock);

	return (rv);
}

/*
 * Free the memory of NDMP message body.  Because we share a common set of XDR
 * ops, this must be done under the session lock.
 */
void
ndmp_free_message(ndmp_session_t *session, ndmp_msg_t *msg)
{
	if (msg->mi_handler == NULL ||
	    msg->mi_body == NULL)
		return;

	(void) mutex_lock(&session->ns_lock);
	session->ns_xdrs.x_op = XDR_FREE;
	if (msg->mi_hdr.message_type == NDMP_MESSAGE_REQUEST) {
		if (msg->mi_handler->mh_xdr_request)
			(*msg->mi_handler->mh_xdr_request)(
			    &session->ns_xdrs, msg->mi_body);
	} else {
		if (msg->mi_handler->mh_xdr_reply)
			(*msg->mi_handler->mh_xdr_reply)(
			    &session->ns_xdrs,
			    msg->mi_body);
	}
	(void) mutex_unlock(&session->ns_lock);

	(void) free(msg->mi_body);
	msg->mi_body = NULL;
}

/*
 * Send an NDMP request.  If 'reply' is non-NULL, then we will wait for a reply.
 * The resulting message must be freed with ndmp_free_message().
 */
int
ndmp_send_request(ndmp_session_t *session, ndmp_message message,
    void *request_data, ndmp_msg_t *reply)
{
	ndmp_header header;
	ndmp_msg_handler_t *handler;
	int err;
	struct timeval time;
	const char *messagestr;

	(void) mutex_lock(&session->ns_lock);

	/* Lookup info necessary for processing this request. */
	handler = ndmp_get_handler(session, message,
	    NDMP_MESSAGE_REQUEST, B_FALSE, &messagestr);
	assert(handler != NULL);
	(void) gettimeofday(&time, 0);

	ndmp_debug(session, "sending message %s", messagestr);

	header.sequence = ++(session->ns_my_sequence);
	header.time_stamp = time.tv_sec;
	header.message_type = NDMP_MESSAGE_REQUEST;
	header.message = message;
	header.reply_sequence = 0;
	header.error = NDMP_NO_ERR;

	/* encode the header and (optionally) message body */
	session->ns_xdrs.x_op = XDR_ENCODE;
	if (!xdr_ndmp_header(&session->ns_xdrs, &header)) {
		if (session->ns_conn_error == 0) {
			ndmp_log_local(session, LOG_ERR,
			    "failed to encode message 0x%x header", message);
		}
		(void) xdrrec_endofrecord(&session->ns_xdrs, 1);
		(void) mutex_unlock(&session->ns_lock);
		return (-1);
	}
	if (handler->mh_xdr_request != NULL && request_data != NULL) {
		if (!(*handler->mh_xdr_request)(&session->ns_xdrs,
		    request_data)) {
			if (session->ns_conn_error == 0) {
				ndmp_log_local(session, LOG_ERR,
				    "failed to encode message 0x%x body",
				    message);
			}
			(void) xdrrec_endofrecord(&session->ns_xdrs, 1);
			(void) mutex_unlock(&session->ns_lock);
			return (-1);
		}
	}
	(void) xdrrec_endofrecord(&session->ns_xdrs, 1);

	/*
	 * Process messages until the reply to this request has been processed.
	 * Certain messages are exempt from this processing, such as
	 * CONNECT_CLOSE, NOTIFY, and LOG messages, and are noted with a NULL
	 * XDR reply callback.
	 */
	if (handler->mh_xdr_reply == NULL) {
		(void) mutex_unlock(&session->ns_lock);
		return (0);
	}

	for (;;) {
		switch (ndmp_process_messages(session, B_TRUE,
		    B_TRUE)) {
		case NDMP_PROC_ERR:
		case NDMP_PROC_REP_ERR:
			/* error */
			(void) mutex_unlock(&session->ns_lock);
			return (-1);

		case NDMP_PROC_REP:
			/* no reply received */
			break;

		case NDMP_PROC_MSG:
			/* reply received */
			if (message !=
			    session->ns_msginfo.mi_hdr.message) {
				ndmp_log_local(session, LOG_ERR,
				    "received unexpected reply 0x%x",
				    session->ns_msginfo.mi_hdr.message);
				ndmp_free_message(session,
				    &session->ns_msginfo);
				(void) mutex_unlock(&session->ns_lock);
				return (-1);
			}

			if (reply != NULL) {
				*reply = session->ns_msginfo;
				session->ns_msginfo.mi_body = NULL;
			} else {
				ndmp_free_message(session,
				    &session->ns_msginfo);
			}

			err = session->ns_msginfo.mi_hdr.error;
			(void) mutex_unlock(&session->ns_lock);

			return (err);

		default:
			/* the above should be the only possible values */
			abort();
		}
	}
}

/*
 * Send an NDMP reply message.  The body is only sent if the error code is
 * NDMP_NO_ERR.
 */
int
ndmp_send_response(ndmp_session_t *session, ndmp_error err,
    void *reply)
{
	ndmp_header header;
	struct timeval time;

	(void) gettimeofday(&time, 0);

	header.sequence = ++(session->ns_my_sequence);
	header.time_stamp = time.tv_sec;
	header.message_type = NDMP_MESSAGE_REPLY;
	header.message = session->ns_msginfo.mi_hdr.message;
	header.reply_sequence = session->ns_msginfo.mi_hdr.sequence;
	header.error = err;

	/* encode the header and (optionally) reply */
	session->ns_xdrs.x_op = XDR_ENCODE;
	if (!xdr_ndmp_header(&session->ns_xdrs, &header)) {
		ndmp_log_local(session, LOG_ERR,
		    "failed to encode reply 0x%x header",
		    header.message);
		(void) xdrrec_endofrecord(&session->ns_xdrs, 1);
		return (-1);
	}
	if (err == NDMP_NO_ERR && reply != NULL &&
	    session->ns_msginfo.mi_handler->mh_xdr_reply) {
		if (!(*session->ns_msginfo.mi_handler->mh_xdr_reply)(
		    &session->ns_xdrs, reply)) {
			ndmp_log_local(session, LOG_ERR,
			    "failed to encode reply 0x%x body",
			    header.message);
			(void) xdrrec_endofrecord(&session->ns_xdrs, 1);
			return (-1);
		}
	}
	(void) xdrrec_endofrecord(&session->ns_xdrs, 1);

	return (0);
}

/*
 * We don't want to modify ndmp_error directly as that's controlled by the NDMP
 * spec, so we just add an override here.  This only has semantic significance
 * for ndmp_recv_msg().  This is different from NDMP_XDR_DECODE_ERR, which
 * implies that we were able to decode the header but not the arguments.
 */
#define	NDMP_HDR_DECODE_ERR (-1)

/*
 * Read the next message.  Returns NDMP_NO_ERR on success, NDMP_HDR_DECODE_ERR
 * if there was an error decoding the header, or an NDMP error code for any
 * other error.
 */
static int
ndmp_recv_msg(ndmp_session_t *session)
{
	bool_t(*xdr_func) (XDR *, ...) = NULL;

	/* Decode the header. */
	session->ns_xdrs.x_op = XDR_DECODE;
	(void) xdrrec_skiprecord(&session->ns_xdrs);
	if (!xdr_ndmp_header(&session->ns_xdrs,
	    &session->ns_msginfo.mi_hdr)) {
		if (session->ns_conn_error == 0)
			ndmp_log_local(session, LOG_ERR,
			    "failed to decode message");
		return (NDMP_HDR_DECODE_ERR);
	}

	/* Lookup info necessary for processing this message. */
	if ((session->ns_msginfo.mi_handler = ndmp_get_handler(session,
	    session->ns_msginfo.mi_hdr.message,
	    session->ns_msginfo.mi_hdr.message_type, B_TRUE,
	    &session->ns_msginfo.mi_messagestr)) == NULL) {
		ndmp_debug(session, "message 0x%x not supported",
		    session->ns_msginfo.mi_hdr.message);
		return (NDMP_NOT_SUPPORTED_ERR);
	}
	session->ns_msginfo.mi_body = 0;

	/*
	 * If the message header indicates an error, there is no associated
	 * body.
	 */
	if (session->ns_msginfo.mi_hdr.error != NDMP_NO_ERR)
		return (NDMP_NO_ERR);

	/* Determine body type */
	if (session->ns_msginfo.mi_hdr.message_type ==
	    NDMP_MESSAGE_REQUEST) {
		if (ndmp_check_auth_required(
		    session->ns_msginfo.mi_hdr.message) &&
		    !session->ns_authorized) {
			ndmp_log_local(session, LOG_ERR,
			    "session not authorized");
			return (NDMP_NOT_AUTHORIZED_ERR);
		}
		if (session->ns_msginfo.mi_handler->mh_sizeof_request >
		    0) {
			xdr_func =
			    session->ns_msginfo.mi_handler->mh_xdr_request;
			if (xdr_func == NULL) {
				ndmp_log_local(session, LOG_ERR,
				    "unsupported request payload "
				    "for message 0x%x",
				    session->ns_msginfo.mi_hdr.message);
				return (NDMP_NOT_SUPPORTED_ERR);
			}
			session->ns_msginfo.mi_body = ndmp_malloc(session,
			    session->ns_msginfo.mi_handler->
			    mh_sizeof_request);
			if (session->ns_msginfo.mi_body == NULL)
				return (NDMP_NO_MEM_ERR);
		}
	} else {
		if (session->ns_msginfo.mi_handler->mh_sizeof_reply > 0) {
			xdr_func =
			    session->ns_msginfo.mi_handler->mh_xdr_reply;
			if (xdr_func == NULL) {
				ndmp_log_local(session, LOG_ERR,
				    "unsupported reply payload "
				    "for message 0x%x",
				    session->ns_msginfo.mi_hdr.message);
				return (NDMP_NOT_SUPPORTED_ERR);
			}
			session->ns_msginfo.mi_body = ndmp_malloc(session,
			    session->ns_msginfo.mi_handler->
			    mh_sizeof_reply);
			if (session->ns_msginfo.mi_body == NULL)
				return (NDMP_NO_MEM_ERR);

			(void) memset(session->ns_msginfo.mi_body, 0,
			    session->ns_msginfo.mi_handler->
			    mh_sizeof_reply);
		}
	}

	/* Decode message arguments if needed */
	if (xdr_func) {
		if (!(*xdr_func)(&session->ns_xdrs,
		    session->ns_msginfo.mi_body)) {
			ndmp_log_local(session, LOG_ERR,
			    "invalid arguments for message 0x%x",
			    session->ns_msginfo.mi_hdr.message);
			free(session->ns_msginfo.mi_body);
			session->ns_msginfo.mi_body = 0;
			return (NDMP_XDR_DECODE_ERR);
		}
	}

	return (NDMP_NO_ERR);
}

/*
 * Processes messages until the stream buffer is empty or a reply is received.
 *
 * This function processes all data in the stream buffer before returning.
 * This allows functions like poll() to be used to determine when new
 * messages have arrived. If only some of the messages in the stream buffer
 * were processed and then poll was called, poll() could block waiting for
 * a message that had already been received and read into the stream buffer.
 *
 * This function processes both request and reply messages.  Request messages
 * are dispatched using the appropriate function from the message handling
 * table.  Only one reply messages may be pending receipt at a time.  A reply
 * message, if received, is placed in session->ns_msginfo before returning
 * to the caller.  Errors are reported if a reply is received but not expected
 * or if more than one reply message is received.
 */
static ndmp_proc_ret_t
ndmp_process_messages(ndmp_session_t *session, boolean_t reply_expected,
    boolean_t stop_on_abort)
{
	ndmp_msg_t reply_msginfo;
	boolean_t reply_read = B_FALSE;
	boolean_t reply_error = B_FALSE;
	int err;
	ndmp_msg_handler_t *handler;
	const char *messagestr;

	(void) memset(&reply_msginfo, 0, sizeof (ndmp_msg_t));

	do {
		(void) memset(&session->ns_msginfo, 0,
		    sizeof (ndmp_msg_t));

		if ((err = ndmp_recv_msg(session)) != NDMP_NO_ERR) {
			if (session->ns_eof)
				return (NDMP_PROC_ERR);

			if (session->ns_data.dd_abort) {
				if (stop_on_abort)
					return (NDMP_PROC_ERR);
				continue;
			}

			if (err == NDMP_HDR_DECODE_ERR) {
				/*
				 * Error occurred decoding the header.  Don't
				 * send a reply since we don't know the message
				 * or if the message was even a request
				 * message.  To be safe, assume that the
				 * message was a reply if a reply was expected.
				 * Need to do this to prevent hanging
				 * ndmp_send_request() waiting for a reply.
				 * Don't set reply_read so that the reply will
				 * be processed if it is received later.
				 */
				if (!reply_read)
					reply_error = B_TRUE;
				continue;
			}

			if (session->ns_msginfo.mi_hdr.message_type
			    != NDMP_MESSAGE_REQUEST) {
				ndmp_debug(session, "received reply: 0x%x",
				    session->ns_msginfo.mi_hdr.message);

				if (!reply_expected || reply_read)
					ndmp_log_local(session, LOG_ERR,
					    "received unexpected reply "
					    "message 0x%x",
					    session->ns_msginfo.mi_hdr.
					    message);

				ndmp_free_message(session,
				    &session->ns_msginfo);

				if (!reply_read) {
					reply_read = B_TRUE;
					reply_error = B_TRUE;
				}
				continue;
			}

			ndmp_debug(session, "received request: 0x%x",
			    session->ns_msginfo.mi_hdr.message);

			(void) ndmp_send_response(session, err, NULL);
			ndmp_free_message(session,
			    &session->ns_msginfo);
			continue;
		}

		handler = session->ns_msginfo.mi_handler;
		assert(handler != NULL);
		messagestr = session->ns_msginfo.mi_messagestr;

		if (session->ns_msginfo.mi_hdr.message_type
		    != NDMP_MESSAGE_REQUEST) {
			ndmp_debug(session, "received reply: %s", messagestr);

			if (!reply_expected || reply_read) {
				ndmp_log_local(session, LOG_ERR,
				    "unexpected reply message %s",
				    messagestr);
				ndmp_free_message(session,
				    &session->ns_msginfo);
				continue;
			}
			reply_read = B_TRUE;
			reply_msginfo = session->ns_msginfo;
			continue;
		}

		ndmp_debug(session, "received request: %s", messagestr);

		/*
		 * The following is needed to catch an improperly constructed
		 * handler table or to deal with an NDMP client that is not
		 * conforming to the negotiated protocol version.
		 */
		if (handler->mh_func == NULL) {
			ndmp_debug(session, "unsupported message %s",
			    messagestr);

			(void) ndmp_send_response(session,
			    NDMP_NOT_SUPPORTED_ERR, NULL);
			ndmp_free_message(session,
			    &session->ns_msginfo);
			continue;
		}

		/*
		 * Call the handler function.
		 * The handler will send any necessary reply.
		 */
		(*handler->mh_func)(session,
		    session->ns_msginfo.mi_body);

		ndmp_free_message(session,
		    &session->ns_msginfo);

	} while (!xdrrec_eof(&session->ns_xdrs) && !session->ns_eof);

	if (session->ns_eof) {
		if (reply_msginfo.mi_body)
			free(reply_msginfo.mi_body);
		return (NDMP_PROC_ERR);
	}

	if (reply_error) {
		if (reply_msginfo.mi_body)
			free(reply_msginfo.mi_body);
		return (NDMP_PROC_REP_ERR);
	}

	if (reply_read) {
		session->ns_msginfo = reply_msginfo;
		return (NDMP_PROC_MSG);
	}

	return (NDMP_PROC_REP);
}
