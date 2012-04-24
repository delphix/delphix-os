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

#ifndef _NDMPD_H
#define	_NDMPD_H

#include <sys/stat.h>
#include <sys/types.h>

#include <alloca.h>
#include <arpa/inet.h>
#include <assert.h>
#include <atomic.h>
#include <crypt.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libinetutil.h>
#include <md5.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <note.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mnttab.h>
#include <sys/mntent.h>
#include <sys/mntio.h>
#include <sys/mtio.h>
#include <sys/queue.h>
#include <sys/scsi/scsi.h>
#include <sys/scsi/impl/uscsi.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/systeminfo.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/uio.h>
#include <syslog.h>
#include <unistd.h>

#include "ndmp.h"
#include "libndmp.h"

#define	NDMP_VMIN		NDMPV3
#define	NDMP_VMIN_STR		"3"
#define	NDMP_VMAX		NDMPV4
#define	NDMP_VMAX_STR		"4"

#define	MAX_RECORD_SIZE		(126*512)
#define	REMOTE_RECORD_SIZE	(60*KILOBYTE)
#define	SCSI_MAX_NAME		32
#define	MD5_CHALLENGE_SIZE	64
#define	MD5_PASS_LIMIT		32

/* Test unit ready */
#define	TUR_WAIT	3000000
#define	TUR_MAX_TRY	3


/* File handler classes */
#define	HC_CLIENT	1
#define	HC_MOVER	2
#define	HC_ALL		0xffffffff

#define	IN_ADDR(x) \
	(*(struct in_addr *)&x)

typedef void *(*funct_t)(void *);

#define	HOSTNAMELEN	256

/*
 * Default maximum permitted sequence number for the token-based backup.
 */
#define	NDMP_MAX_TOKSEQ	9

/* All 1's binary maximum mover window */
#define	MAX_WINDOW_SIZE	0xffffffffffffffffULL

#define	NDMP_FREE(cp)	{ free((char *)(cp)); (cp) = NULL; }

#define	NDMP_SVAL(cp)	((cp) ? (cp) : "NULL")

#define	NDMP_SETENV(env, nm, val) \
	{ \
		env->name = nm; \
		env->value = val; \
		env++; \
	}

#define	NDMP_CL_ADDR_LEN	24
#define	NDMP_TCP_ADDR_SIZE	32
#define	NDMP_TAPE_DEV_NAME	256

#define	NLP_READY	1

#define	AUTH_REQUIRED	B_TRUE
#define	AUTH_NOT_REQUIRED	B_FALSE
#define	NDMP_EOM_MAGIC	"PRCMEOM"
#define	KILOBYTE	1024

#define	INT_MAXCMD	12

typedef struct scsi_device {
	struct scsi_device	*sd_next;
	unsigned int		sd_sid;
	unsigned int		sd_lun;
	unsigned int		sd_requested_max_active;
	unsigned int		sd_granted_max_active;
	unsigned int		sd_n_active;
	unsigned int		sd_type; /* SCSI device type */
	char			sd_name[256];
	char			sd_vendor[8 + 1];
	char			sd_id[16 + 1];
	char			sd_rev[4 + 1];
	char			sd_serial[16 + 1];
	char			sd_wwn[32 + 1];
} scsi_device_t;

/* buffers */

#define	NDMP_TAPE_BUFFERS	10	/* number of rotating tape buffers */
#define	NDMP_LINE_SIZE		128	/* size of text messages */

#define	NDMP_BACKUP_RUN		0x00000001
#define	NDMP_RESTORE_RUN		0x00000002
#define	NDMP_STOP		0x00000009	/* graceful stop */
#define	NDMP_ABORT		0x99999999	/* abandon the run */

/*
 * Synchronization flags used when launching the buffer threads.
 */
#define	NDMP_TAPE_READER		0x00000001
#define	NDMP_TAPE_WRITER		0x00000002
#define	NDMP_SOCK_READER		0x00000004
#define	NDMP_SOCK_WRITER		0x00000008

#define	NDMP_MAX_SELECTIONS	64

typedef struct	ndmp_buffer {
	char	*nb_buffer_data;	/* area to be used for I/O */
	long	nb_buffer_size;	/* number of valid bytes in the buffer */
	long	nb_buffer_spot;	/* current location in the I/O buffer */
	longlong_t nb_seek_spot;	/* for BACKUP */
				/* where in the file this buffer stops. */
				/* this is used for the Multi Volume */
				/* Header record. */
	longlong_t nb_file_size;	/* for BACKUP */
					/* how much of the file is left. */
	int	nb_full	: 1,
		nb_eot	: 1,
		nb_eof	: 1;
	int	nb_errno;	/* I/O error values */
} ndmp_buffer_t;

/* Connection data structure. */
typedef struct ndmp_msg {
	ndmp_header mi_hdr;
	struct ndmp_msg_handler *mi_handler;
	const char *mi_messagestr;
	void *mi_body;
} ndmp_msg_t;

typedef enum {
	NDMP_MESSAGE_CONFIG = 0x1,
	NDMP_MESSAGE_SCSI = 0x2,
	NDMP_MESSAGE_TAPE = 0x3,
	NDMP_MESSAGE_DATA = 0x4,
	NDMP_MESSAGE_NOTIFY = 0x5,
	NDMP_MESSAGE_LOG  = 0x6,
	NDMP_MESSAGE_FH = 0x7,
	NDMP_MESSAGE_CONNECT = 0x9,
	NDMP_MESSAGE_MOVER = 0xA,
} ndmp_message_class_t;

struct ndmp_session;

typedef void ndmp_msg_handler_func_t(struct ndmp_session *, void *);


typedef struct ndmp_msg_handler {
	ndmp_msg_handler_func_t *mh_func;
	bool_t(*mh_xdr_request) (XDR *xdrs, ...);
	int mh_sizeof_request;
	bool_t(*mh_xdr_reply) (XDR *xdrs, ...);
	int mh_sizeof_reply;
} ndmp_msg_handler_t;

typedef struct ndmp_handler {
	int hd_cnt;
	struct hd_messages {
		ndmp_message hm_message;
		const char *hm_messagestr;
		boolean_t hm_auth_required;
		ndmp_msg_handler_t hm_msg_v[NDMP_VMAX - NDMP_VMIN + 1];
	} hd_msgs[INT_MAXCMD];
} ndmp_handler_t;


#define	NDMPD_SELECT_MODE_READ		1
#define	NDMPD_SELECT_MODE_WRITE		2

typedef void ndmp_file_handler_func_t(struct ndmp_session *, int, ulong_t);


typedef struct mem_ndmp_name_v3 {
	char *nm3_opath;
	char *nm3_dpath;
	char *nm3_newnm;
	u_longlong_t nm3_node;
	u_longlong_t nm3_fh_info;
	ndmp_error nm3_err;
} mem_ndmp_name_v3_t;

typedef struct ndmp_file_handler {
	int fh_fd;
	ulong_t fh_mode;
	ulong_t fh_class;
	void *fh_cookie;
	ndmp_file_handler_func_t *fh_func;
	struct ndmp_file_handler *fh_next;
} ndmp_file_handler_t;

typedef struct ndmp_session_scsi_desc {
	int sd_is_open;
	int sd_devid;
	boolean_t sd_valid_target_set;
	int sd_sid;
	int sd_lun;
	char sd_adapter_name[SCSI_MAX_NAME];
} ndmp_session_scsi_desc_t;

typedef struct ndmp_session_tape_desc {
	int td_fd;			/* tape device file descriptor */
	ulong_t td_record_count;	/* number of records written */
	ndmp_tape_open_mode td_mode;	/* tape device open mode */
	u_longlong_t td_pos;	/* current position on the current tape */
	int td_sid;
	int td_lun;
	char td_adapter_name[SCSI_MAX_NAME];
	ulong_t td_eom_seen:1,
		td_io_err:1,
		td_write:1;
} ndmp_session_tape_desc_t;

typedef struct ndmp_session_mover_desc {
	ndmp_mover_state md_state;	/* current state */
	ndmp_mover_mode md_mode;	/* current mode */
	ndmp_mover_pause_reason md_pause_reason;	/* current reason */
	ndmp_mover_halt_reason md_halt_reason;	/* current reason */
	u_longlong_t md_data_written;	/* total written to tape */
	u_longlong_t md_seek_position;	/* current seek position */
	u_longlong_t md_bytes_left_to_read; /* #bytes to end of seek window */
	u_longlong_t md_window_offset;	/* valid data window begin */
	u_longlong_t md_window_length;	/* valid data window length */
	u_longlong_t md_position;	/* current data stream pos */
	boolean_t md_pre_cond;		/* used for precondition checks */
	ulong_t md_record_size;	/* tape I/O record size */
	ulong_t md_record_num;	/* current record num */
	int md_listen_sock;		/* data conn listen socket */
	int md_sock;		/* data conn socket */
	ulong_t md_r_index;		/* buffer read  index */
	ulong_t md_w_index;		/* buffer write index */
	char *md_buf;		/* data buffer */
	ulong_t md_discard_length;	/* bytes to discard */
	ndmp_addr_v3 md_data_addr;
	ndmp_addr_v4 md_data_addr_v4;
} ndmp_session_mover_desc_t;

typedef struct ndmp_session_data_desc {
	/*
	 * Common fields.
	 */
	ndmp_data_operation dd_operation;	/* current operation */
	boolean_t dd_abort;		/* abort operation flag */
	boolean_t dd_io_ready;		/* mover sock read for I/O */
	ndmp_pval *dd_env;	/* environment from backup or recover request */
	mutex_t dd_env_lock;	/* environment lock */
	ulong_t dd_env_len;		/* environment length */
	ulong_t dd_nlist_len;	/* recover file list length */
	int dd_sock;		/* listen and data socket */
	u_longlong_t dd_read_offset;	/* data read seek offset */
	u_longlong_t dd_read_length;	/* data read length */
	u_longlong_t dd_data_size;	/* data size to be backed up */
	u_longlong_t dd_bytes_processed;
	u_longlong_t dd_est_bytes_remaining;
	ulong_t dd_est_time_remaining;

	ndmp_data_state dd_state;	/* current state */
	ndmp_data_halt_reason dd_halt_reason;		/* current reason */
	ndmp_name *dd_nlist;	/* recover file list */
	ndmp_mover_addr dd_mover;	/* mover address */
	mem_ndmp_name_v3_t *dd_nlist_v3;
	ndmp_addr_v3 dd_data_addr;
	int dd_listen_sock;	/* socket for listening for remote */
				/* mover connections */
	u_longlong_t dd_bytes_left_to_read;
	u_longlong_t dd_position;
	u_longlong_t dd_discard_length;
	ndmp_addr_v4 dd_data_addr_v4;

	int dd_errno; /* errno result from reading/writing to socket */
} ndmp_session_data_desc_t;

typedef struct ndmp_session_notify_state {
	mutex_t ns_lock;
	cond_t ns_cv;
	ndmp_notification_t *ns_list;
	boolean_t ns_failed;
} ndmp_session_notify_state_t;

typedef struct ndmp_session {
	ushort_t ns_version;	/* connection protocol version */

	struct ndmp_session *ns_next;

	struct ndmp_server *ns_server;
	struct ndmp_client *ns_client;
	ndmp_common_conf_t *ns_conf;
	ndmp_session_scsi_desc_t ns_scsi;
	ndmp_session_tape_desc_t ns_tape;
	ndmp_session_mover_desc_t ns_mover;
	ndmp_session_data_desc_t ns_data;
	ndmp_session_notify_state_t ns_notify;
	ndmp_file_handler_t *ns_file_handler_list; /* for I/O multiplexing */
	mutex_t ns_file_handler_lock;
	boolean_t ns_global;

	uint32_t ns_logid;

	unsigned char ns_challenge[MD5_CHALLENGE_SIZE];  /* For MD5 */
	boolean_t ns_set_ext_list;

	ndmp_fs_info_v3 *ns_fsinfo;
	size_t ns_fsinfo_alloc;
	size_t ns_fsinfo_count;

	boolean_t ns_running;		/* backup or restore running */

	/* connection properties */
	int ns_sock;
	XDR ns_xdrs;
	ulong_t ns_my_sequence;
	boolean_t ns_authorized;
	boolean_t ns_eof;
	int ns_conn_error;
	ndmp_msg_t ns_msginfo; /* received request or reply message */
	boolean_t ns_version_known;
	char ns_remoteaddr[INET6_ADDRSTRLEN + 6];
	mutex_t ns_lock;

} ndmp_session_t;

struct ndmp_client;
struct ndmp_server;

typedef struct ndmp_session_list {
	ndmp_session_t *nsl_head;
	int nsl_count;
	mutex_t nsl_lock;
	cond_t nsl_cv;
} ndmp_session_list_t;

typedef enum {
	NDMP_DAR_SUPPORT = 0,
	NDMP_MAXSEQ_ENV,
	NDMP_MAX_VERSION,
	NDMP_MIN_VERSION,
	NDMP_SOCKET_CSS,
	NDMP_SOCKET_CRS,
	NDMP_MOVER_RECSIZE,
	NDMP_TCP_PORT,
	NDMP_DRIVE_TYPE,
	NDMP_LOCAL_TAPE,
	NDMP_TAPE_TEST,
	NDMP_MAXALL
} ndmp_prop_t;

typedef struct ndmp_server {
	ndmp_server_conf_t *ns_conf;	/* client-provided configuration */
	int ns_listen_socket;		/* main listener socket */
	pthread_t ns_listen_thread;	/* main listener thread */
	ndmp_session_list_t ns_session_list;	/* connection list */
	ndmp_session_t ns_global_session; /* session for callbacks */
	const char *ns_props[NDMP_MAXALL]; /* server properties */
	boolean_t ns_shutdown;		/* shutdown requested */
	mutex_t ns_ndmp_lock;		/* tape/SCSI lock */
	scsi_device_t *ns_scsi_devices;	/* scsi devices */
} ndmp_server_t;

typedef struct ndmp_client {
	ndmp_client_conf_t *nc_conf;	/* client-provided configuration */
	ndmp_session_list_t nc_session_list;	/* connection list */
	ndmp_session_t nc_global_session; /* session for callbacks */
	const char *nc_props[NDMP_MAXALL]; /* client properties */
	boolean_t nc_shutdown;		/* client shutdown requested */
} ndmp_client_t;

extern int ndmp_load_prop(ndmp_session_t *);
extern const char *ndmp_get_prop(ndmp_session_t *, ndmp_prop_t);
extern int ndmp_get_prop_int(ndmp_session_t *, ndmp_prop_t);
extern boolean_t ndmp_get_prop_boolean(ndmp_session_t *, ndmp_prop_t);

/*
 * NDMP request handler functions.
 */

/* Config */
ndmp_msg_handler_func_t ndmp_config_get_host_info_v3;
ndmp_msg_handler_func_t ndmp_config_get_butype_info_v3;
ndmp_msg_handler_func_t ndmp_config_get_connection_type_v3;
ndmp_msg_handler_func_t ndmp_config_get_auth_attr_v3;
ndmp_msg_handler_func_t ndmp_config_get_fs_info_v3;
ndmp_msg_handler_func_t ndmp_config_get_tape_info_v3;
ndmp_msg_handler_func_t ndmp_config_get_scsi_info_v3;
ndmp_msg_handler_func_t ndmp_config_get_server_info_v3;

ndmp_msg_handler_func_t ndmp_config_get_butype_info_v4;
ndmp_msg_handler_func_t ndmp_config_get_ext_list_v4;
ndmp_msg_handler_func_t ndmp_config_set_ext_list_v4;

/* Scsi */
ndmp_msg_handler_func_t ndmp_scsi_close_v3;
ndmp_msg_handler_func_t ndmp_scsi_get_state_v3;
ndmp_msg_handler_func_t ndmp_scsi_reset_device_v3;
ndmp_msg_handler_func_t ndmp_scsi_reset_bus_v3;
ndmp_msg_handler_func_t ndmp_scsi_execute_cdb_v3;
ndmp_msg_handler_func_t ndmp_scsi_open_v3;
ndmp_msg_handler_func_t ndmp_scsi_set_target_v3;

/* Tape */
ndmp_msg_handler_func_t ndmp_tape_close_v3;
ndmp_msg_handler_func_t ndmp_tape_mtio_v3;
ndmp_msg_handler_func_t ndmp_tape_execute_cdb_v3;
ndmp_msg_handler_func_t ndmp_tape_open_v3;
ndmp_msg_handler_func_t ndmp_tape_get_state_v3;
ndmp_msg_handler_func_t ndmp_tape_write_v3;
ndmp_msg_handler_func_t ndmp_tape_read_v3;

ndmp_msg_handler_func_t ndmp_tape_close_v4;

/* Data */
ndmp_msg_handler_func_t ndmp_data_get_env_v3;
ndmp_msg_handler_func_t ndmp_data_get_state_v3;
ndmp_msg_handler_func_t ndmp_data_connect_v3;
ndmp_msg_handler_func_t ndmp_data_listen_v3;
ndmp_msg_handler_func_t ndmp_data_stop_v3;
ndmp_msg_handler_func_t ndmp_data_abort_v3;
ndmp_msg_handler_func_t ndmp_data_start_recover_v3;
ndmp_msg_handler_func_t ndmp_data_start_backup_v3;

ndmp_msg_handler_func_t ndmp_data_get_env_v4;
ndmp_msg_handler_func_t ndmp_data_get_state_v4;
ndmp_msg_handler_func_t ndmp_data_connect_v4;
ndmp_msg_handler_func_t ndmp_data_listen_v4;

/* Connect */
ndmp_msg_handler_func_t ndmp_connect_open_v3;
ndmp_msg_handler_func_t ndmp_connect_client_auth_v3;
ndmp_msg_handler_func_t ndmp_connect_close_v3;

/* Mover */
ndmp_msg_handler_func_t ndmp_mover_stop_v3;
ndmp_msg_handler_func_t ndmp_mover_close_v3;
ndmp_msg_handler_func_t ndmp_mover_get_state_v3;
ndmp_msg_handler_func_t ndmp_mover_listen_v3;
ndmp_msg_handler_func_t ndmp_mover_continue_v3;
ndmp_msg_handler_func_t ndmp_mover_abort_v3;
ndmp_msg_handler_func_t ndmp_mover_set_window_v3;
ndmp_msg_handler_func_t ndmp_mover_read_v3;
ndmp_msg_handler_func_t ndmp_mover_set_record_size_v3;
ndmp_msg_handler_func_t ndmp_mover_connect_v3;

ndmp_msg_handler_func_t ndmp_mover_get_state_v4;
ndmp_msg_handler_func_t ndmp_mover_listen_v4;
ndmp_msg_handler_func_t ndmp_mover_connect_v4;

/* Notify */
ndmp_msg_handler_func_t ndmp_notify_data_halted_v3;
ndmp_msg_handler_func_t ndmp_notify_data_halted_v4;
ndmp_msg_handler_func_t ndmp_notify_connection_status_v3;
ndmp_msg_handler_func_t ndmp_notify_mover_halted_v3;
ndmp_msg_handler_func_t ndmp_notify_mover_halted_v4;
ndmp_msg_handler_func_t ndmp_notify_mover_paused_v3;
ndmp_msg_handler_func_t ndmp_notify_data_read_v3;

/* Log */
ndmp_msg_handler_func_t ndmp_log_file_v3;
ndmp_msg_handler_func_t ndmp_log_message_v3;
ndmp_msg_handler_func_t ndmp_log_message_v4;

typedef void ndmp_func_t(ndmp_session_t *, void *);

/*
 * Utility functions form ndmp_data.c.
 */

/*
 * Utility functions from ndmp_mover.c.
 */
extern int ndmp_mover_init(ndmp_session_t *);
extern void ndmp_mover_cleanup(ndmp_session_t *);
extern ndmp_error ndmp_mover_connect(ndmp_session_t *, ndmp_mover_mode);
extern void ndmp_mover_error(ndmp_session_t *, ndmp_mover_halt_reason);
extern int ndmp_mover_seek(ndmp_session_t *, u_longlong_t, u_longlong_t);
extern int ndmp_remote_write(ndmp_session_t *, char *, ulong_t);

extern void ndmp_mover_shut_down(ndmp_session_t *);
extern void ndmp_mover_error(ndmp_session_t *, ndmp_mover_halt_reason);
extern int ndmp_local_write_v3(ndmp_session_t *, char *, ulong_t);
extern int ndmp_local_read_v3(ndmp_session_t *, char *, ulong_t);
extern int ndmp_mover_wait(ndmp_session_t *);
extern void ndmp_write_eom(ndmp_session_t *, int);

/*
 * Global variables from ndmp_util.c
 */
extern int ndmp_rbs;
extern int ndmp_sbs;

/*
 * Utility functions from ndmp_util.c.
 */
extern int ndmp_select(ndmp_session_t *, boolean_t, ulong_t);
extern ndmp_error ndmp_save_env(ndmp_session_t *, ndmp_pval *,
    ulong_t);
extern void ndmp_free_env(ndmp_session_t *);
extern void ndmp_free_nlist(ndmp_session_t *);
extern int ndmp_add_file_handler(ndmp_session_t *,
    void *, int, ulong_t, ulong_t, ndmp_file_handler_func_t *);
extern void ndmp_remove_file_handler(ndmp_session_t *, int);
extern void ndmp_send_reply(ndmp_session_t *, void *);

extern int ndmp_mtioctl(ndmp_session_t *, int, int, int);

extern u_longlong_t quad_to_long_long(ndmp_u_quad);
extern ndmp_u_quad long_long_to_quad(u_longlong_t);

extern void ndmp_set_socket_nodelay(int);
extern void ndmp_set_socket_snd_buf(ndmp_session_t *, int, int);
extern void ndmp_set_socket_rcv_buf(ndmp_session_t *, int, int);

extern boolean_t is_buffer_erroneous(ndmp_session_t *, ndmp_buffer_t *);
extern void ndmp_execute_cdb(ndmp_session_t *, char *,
    int, int, ndmp_execute_cdb_request *);

extern boolean_t is_tape_unit_ready(ndmp_session_t *, char *, int);

extern int ndmp_open_list_add(ndmp_session_t *, char *, int, int, int);
extern int ndmp_open_list_del(char *, int, int);
extern void ndmp_open_list_release(ndmp_session_t *);

extern char *cctime(time_t *);
extern char *ndmp_new_job_name(char *);
extern char *ndmp_mk_temp(char *);
extern char *ndmp_make_bk_dir_path(char *, char *);
extern char **ndmp_make_exc_list(void);
extern void ndmp_sort_nlist_v3(ndmp_session_t *);
extern ndmp_error ndmp_save_nlist_v3(ndmp_session_t *, ndmp_name_v3 *,
    ulong_t);
extern void ndmp_free_nlist_v3(ndmp_session_t *);
extern int ndmp_create_socket(ndmp_session_t *, ulong_t *, ushort_t *);
extern int ndmp_connect_sock_v3(ndmp_session_t *, ulong_t, ushort_t);
extern void ndmp_copy_addr_v3(ndmp_addr_v3 *, ndmp_addr_v3 *);
extern int ndmp_copy_addr_v4(ndmp_session_t *, ndmp_addr_v4 *, ndmp_addr_v4 *);
extern char *ndmp_addr2str_v3(ndmp_addr_type);
extern boolean_t ndmp_valid_v3addr_type(ndmp_addr_type);
extern char *ndmp_get_relative_path(char *, char *);

extern boolean_t ndmp_fhinode;
extern void randomize(unsigned char *, int);

extern int tape_open(char *, int);

void ndmp_get_file_entry_type(int, ndmp_file_type *);

extern int ndmp_device_init(ndmp_server_t *);
extern void ndmp_device_fini(ndmp_server_t *);

extern void scsi_find_sid_lun(ndmp_session_t *, char *devname, int *sid,
    int *lid);
extern boolean_t scsi_dev_exists(ndmp_session_t *, int, int);
extern int scsi_get_devtype(ndmp_session_t *, int, int);
extern boolean_t ndmp_open_list_exists(char *, int, int);

extern void *ndmp_malloc(ndmp_session_t *, size_t);
extern void *ndmp_realloc(ndmp_session_t *, void *, size_t);
extern char *ndmp_strdup(ndmp_session_t *, const char *);

extern void ndmp_log(ndmp_session_t *, int, const char *, ...);
extern void ndmp_log_local(ndmp_session_t *, int, const char *, ...);
extern void ndmp_debug(ndmp_session_t *, const char *, ...);


extern ndmp_handler_t ndmp_msghdl_tab[];

extern void ndmp_create_md5_digest(unsigned char *, const char *,
    unsigned char *);
extern void ndmp_asprintf(char *buf, const char *fmt, ...);
extern void ndmp_vasprintf(char *buf, const char *fmt, va_list ap);

/* ndmp_comm.c */
extern void *ndmp_server_run(void *);
extern ndmp_session_t *ndmp_connect(ndmp_client_t *, const char *, int);
extern int ndmp_process_requests(ndmp_session_t *, boolean_t);
extern void ndmp_free_message(ndmp_session_t *, ndmp_msg_t *);
extern int ndmp_send_response(ndmp_session_t *, ndmp_error, void *);
extern int ndmp_send_request(ndmp_session_t *,
    ndmp_message, void *, ndmp_msg_t *);

/* ndmp_data.c */
extern void ndmp_data_cleanup(ndmp_session_t *);
extern void ndmp_data_init(ndmp_session_t *);
extern void ndmp_data_error(ndmp_session_t *, ndmp_data_halt_reason);
extern int ndmp_remote_read_v3(ndmp_session_t *, char *, ssize_t);

/* ndmp_notify.c */
extern ndmp_notification_t *ndmp_notify_wait(ndmp_session_t *, uint_t,
    int *);

/* ndmp_session.c */
extern ndmp_session_t *ndmp_session_create(ndmp_session_t *, int);
extern void ndmp_session_close(ndmp_session_t *);
extern void ndmp_session_list_teardown(ndmp_session_list_t *);
extern void ndmp_session_failed(ndmp_session_t *, int);
extern void ndmp_session_data_stop(ndmp_session_t *);

#define	NDMP_ASPRINTF(bufp, fmt, args...) 				\
	(*(bufp) = alloca(snprintf(NULL, 0, (fmt), ## args) + 1),	\
	ndmp_asprintf((*bufp), (fmt), ## args))

#define	NDMP_VASPRINTF(bufp, fmt, ap)					\
	(*(bufp) = alloca(vsnprintf(NULL, 0, (fmt), ap) + 1),		\
	ndmp_vasprintf((*bufp), (fmt), ap))

#endif /* _NDMPD_H */
