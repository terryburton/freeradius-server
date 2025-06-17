/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_kafka.c
 * @brief kafka server authentication.
 *
 * @copyright 2020 The FreeRADIUS server project
 * @copyright 2020 Network RADIUS SAS (legal@networkradius.com)
 */
RCSID("$Id$")

#include <time.h>
#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/global_lib.h>
#include <freeradius-devel/server/module_rlm.h>
#include <freeradius-devel/util/slab.h>
#include <librdkafka/rdkafka.h>

#define NO_DELIVERY_REPORT INT_MIN

static fr_dict_t const *dict_radius; /*dictionary for radius protocol*/

extern fr_dict_autoload_t rlm_kafka_dict[];
fr_dict_autoload_t	  rlm_kafka_dict[] = { { .out = &dict_radius, .proto = "radius" }, { NULL } };

extern fr_dict_attr_autoload_t rlm_kafka_dict_attr[];
fr_dict_attr_autoload_t	       rlm_kafka_dict_attr[] = {
	       { NULL },
};

extern global_lib_autoinst_t const *const rlm_kafka_lib[];
global_lib_autoinst_t const *const	  rlm_kafka_lib[] = { GLOBAL_LIB_TERMINATOR };

typedef struct {
	char const *topic;
    char const *brokers;
} rlm_kafka_t;

typedef struct {
	char const *abc;
} rlm_kafka_thread_t;

/*
 *	A mapping of configuration file names to internal variables.
 */

static const conf_parser_t module_config[] = {
	{ FR_CONF_OFFSET("topic", rlm_kafka_t, topic) },
    { FR_CONF_OFFSET("brokers", rlm_kafka_t, brokers) },
	CONF_PARSER_TERMINATOR
};

static void kafka_io_module_signal(module_ctx_t const *mctx, request_t *request, UNUSED fr_signal_t action)
{
	rlm_kafka_thread_t *t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);
	(void)t;
	(void)request;
}

static void dr_msg_cb(UNUSED rd_kafka_t *rk, const rd_kafka_message_t *rkmessage, UNUSED void *opaque)
{
	/* rd_kafka_resp_err_t *status; */
	int32_t		     broker_id = -1;
	const char	    *persisted = "unknown";
printf("in kafka callback\n");

#if 0
	if (rkmessage->_private) {
		printf("returning from kafka callback at _private check\n");
		status	= (rd_kafka_resp_err_t *)rkmessage->_private;
		*status = rkmessage->err;
		return;
	}

	if (!rkmessage->err) {
		printf("returning from kafka callback at err check\n");
		return;
	}
#endif
printf("RD_KAFKA_VERSION: 0x0%x\n",RD_KAFKA_VERSION);

#if RD_KAFKA_VERSION >= 0x010500ff
	broker_id = rd_kafka_message_broker_id(rkmessage);
#endif

#if RD_KAFKA_VERSION >= 0x010000ff
	switch (rd_kafka_message_status(rkmessage)) {
	case RD_KAFKA_MSG_STATUS_PERSISTED:
		persisted = "definitely";
		break;

	case RD_KAFKA_MSG_STATUS_NOT_PERSISTED:
		persisted = "not";
		break;

	case RD_KAFKA_MSG_STATUS_POSSIBLY_PERSISTED:
		persisted = "possibly";
		break;
	}
#endif
	printf("before ERROR call in kafka callback\n");
	/*ERROR("Kafka delivery report '%s' for key: %.*s (%s persisted to broker %" PRId32 ")\n",*/
	ERROR("Kafka delivery report '%s' for payload: (size:%d) (%s) (%s persisted to broker %" PRId32 ")\n",
	      rd_kafka_err2str(rkmessage->err), (int)rkmessage->len, (char *)rkmessage->payload, persisted, broker_id);
	printf("end of kafka callback\n");
}

/*
 *	Called when the IMAP server responds
 *	It checks if the response was CURLE_OK
 *	If it wasn't we returns REJECT, if it was we returns OK
 */
static unlang_action_t CC_HINT(nonnull)
	mod_authenticate_resume(rlm_rcode_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_kafka_t const *inst = talloc_get_type_abort_const(mctx->mi->data, rlm_kafka_t);

	printf("called kafka mod_authenticate_resume\n\n\n\n");

	(void)request;
	(void)inst;
	RETURN_MODULE_OK;
}

/*
 *	Checks that there is a User-Name and User-Password field in the request
 *	Checks that User-Password is not Blank
 *	Sets the: username, password
 *		website URI
 *		timeout information
 *		and TLS information
 *
 *	Then it queues the request and yields until a response is given
 *	When it responds, mod_authenticate_resume is called.
 */
static unlang_action_t CC_HINT(nonnull(1, 2))
	mod_kafka_publish(rlm_rcode_t *p_result, module_ctx_t const *mctx, request_t *request)
{
    rlm_kafka_t	   *inst = talloc_get_type_abort(mctx->mi->data, rlm_kafka_t);
	rlm_kafka_thread_t *t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);

    rd_kafka_t	   *rk; /* Kafka producer handle */
	rd_kafka_conf_t	   *kconf; /* Kafka configuration */
	rd_kafka_topic_t   *rkt; /* Topic handle */
	rd_kafka_resp_err_t err; /* Error code */
	rd_kafka_resp_err_t status = NO_DELIVERY_REPORT;

	time_t	   current_time;
	struct tm *time_info;
	char	   message[80];


    printf("in mod_kafka_publish():\n");
    printf("topic: %s\n",inst->topic);
    printf("brokers: %s\n",inst->brokers);

	time(&current_time);
	time_info = localtime(&current_time);

	strftime(message, sizeof(message), "sent test message from freeradius at %Y-%m-%d %H:%M:%S", time_info);


	(void)t;
	(void)p_result;

	kconf = rd_kafka_conf_new();
	rd_kafka_conf_set_dr_msg_cb(kconf, dr_msg_cb);

	if (rd_kafka_conf_set(kconf, "bootstrap.servers", inst->brokers, NULL, 0) != RD_KAFKA_CONF_OK) {
		fprintf(stderr, "Failed to set broker list\n");
		exit(1);
	}


	rk = rd_kafka_new(RD_KAFKA_PRODUCER, kconf, NULL, 0);
	if (rk == NULL) {
		fprintf(stderr, "Failed to create Kafka producer\n");
		exit(1);
	}


	rkt = rd_kafka_topic_new(rk, inst->topic, NULL);
	if (rkt == NULL) {
		fprintf(stderr, "Failed to create Kafka topic\n");
		rd_kafka_destroy(rk);
		exit(1);
	}


	err = rd_kafka_produce(rkt, RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_COPY, message, strlen(message), NULL, 0,
			       NULL);
	if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
		fprintf(stderr, "Failed to produce message: %s\n", rd_kafka_err2str(err));
		rd_kafka_topic_destroy(rkt);
		rd_kafka_destroy(rk);
		exit(1);
	}

	/* rd_kafka_flush(rk, 10000);  */

	if (err == RD_KAFKA_RESP_ERR_NO_ERROR) {
		while (status == NO_DELIVERY_REPORT) {

			status = rd_kafka_poll(rk, 1000);
			printf("in kafka module rd_kafka_poll() returned %d\n", status);
		}
		/*    err = status; */
	}
printf("kafka module poll finished\n");
	rd_kafka_topic_destroy(rkt);
	rd_kafka_destroy(rk);
printf("kafka module destroy() calls finished\n");
	return unlang_module_yield(request, mod_authenticate_resume, kafka_io_module_signal, ~FR_SIGNAL_CANCEL, NULL);
}


static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_kafka_t		*inst = talloc_get_type_abort(mctx->mi->data,rlm_kafka_t);
	printf("kafka module mod_instantiate()\n\n\n\n");
	(void)inst;
	return 0;
}

static int mod_detach(module_detach_ctx_t const *mctx)
{
	rlm_kafka_t		*inst = talloc_get_type_abort(mctx->mi->data, rlm_kafka_t);
printf("kafka module mod_detach()\n\n\n\n");
	(void)inst;
	return 0;
}


/*
 *	Initialize a new thread with a curl instance
 */
static int mod_thread_instantiate(module_thread_inst_ctx_t const *mctx)
{
	rlm_kafka_t	   *inst = talloc_get_type_abort(mctx->mi->data, rlm_kafka_t);
	rlm_kafka_thread_t *t	 = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);
	printf("kafka module mod_thread_instantiate()\n\n\n\n");
	(void)inst;
	(void)t;
	return 0;
}

/*
 *	Close the thread and free the memory
 */
static int mod_thread_detach(module_thread_inst_ctx_t const *mctx)
{
	rlm_kafka_thread_t *t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);
	printf("in kafka module. calling mod_thread_detach()\n");
	(void)t;
	return 0;
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to MODULE_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
extern module_rlm_t rlm_kafka;
module_rlm_t rlm_kafka = {
	.common = {
		.magic		        = MODULE_MAGIC_INIT,
		.name		        = "kafka",
		.inst_size	        = sizeof(rlm_kafka_t),
		.thread_inst_size   	= sizeof(rlm_kafka_thread_t),
		.config		        = module_config,
		.instantiate		= mod_instantiate,
		.detach				= mod_detach,
		.thread_instantiate 	= mod_thread_instantiate,
		.thread_detach      	= mod_thread_detach,
	},
	.method_group = {
		.bindings = (module_method_binding_t[]){
			{ .section = SECTION_NAME("authorize", CF_IDENT_ANY), .method = mod_kafka_publish },
			MODULE_BINDING_TERMINATOR
		}
	}
};
