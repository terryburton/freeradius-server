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
	char const *abc;
} rlm_kafka_t;

typedef struct {
	char const *abc;
} rlm_kafka_thread_t;

/*
 *	A mapping of configuration file names to internal variables.
 */
static const conf_parser_t module_config[] = { CONF_PARSER_TERMINATOR };

static void kafka_io_module_signal(module_ctx_t const *mctx, request_t *request, UNUSED fr_signal_t action)
{
	rlm_kafka_thread_t *t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);
	(void)t;
	(void)request;
}

static void dr_msg_cb(UNUSED rd_kafka_t *rk, const rd_kafka_message_t *rkmessage, UNUSED void *opaque)
{
	rd_kafka_resp_err_t *status;
	int32_t		     broker_id = -1;
	const char	    *persisted = "unknown";

	if (rkmessage->_private) {
		status	= (rd_kafka_resp_err_t *)rkmessage->_private;
		*status = rkmessage->err;
		return;
	}

	if (!rkmessage->err) return;

#if RD_KAFKA_VERSION >= 0x010500ff
	broker_id = rd_kafka_message_broker_id(rkmessage);
#endif

#if RD_KAFKA_VERSION >= 0x010000ff
	switch (rd_kafka_message_status(rkmessage)) {
	case RD_KAFKA_MSG_STATUS_PERSISTED:
		persisted = "definately";
		break;

	case RD_KAFKA_MSG_STATUS_NOT_PERSISTED:
		persisted = "not";
		break;

	case RD_KAFKA_MSG_STATUS_POSSIBLY_PERSISTED:
		persisted = "possibly";
		break;
	}
#endif

	ERROR("Kafka delivery report '%s' for key: %.*s (%s persisted to broker %" PRId32 ")\n",
	      rd_kafka_err2str(rkmessage->err), (int)rkmessage->key_len, (char *)rkmessage->key, persisted, broker_id);
}

static int send_kafka_message()
{
	rd_kafka_t	   *rk; /* Kafka producer handle */
	rd_kafka_conf_t	   *kconf; /* Kafka configuration */
	rd_kafka_topic_t   *rkt; /* Topic handle */
	rd_kafka_resp_err_t err; /* Error code */
	rd_kafka_resp_err_t status = NO_DELIVERY_REPORT;

	char brokers[] = "localhost:9092";
	char topic[]   = "kafka-topic-test";

	time_t	   current_time;
	struct tm *time_info;
	char	   message[80];

	time(&current_time);
	time_info = localtime(&current_time);

	strftime(message, sizeof(message), "sent test message from freeradius at %Y-%m-%d %H:%M:%S", time_info);

	printf("calling send_kafka_message(%s)\n\n", message);

	/* Create Kafka configuration */
	kconf = rd_kafka_conf_new();
	rd_kafka_conf_set_dr_msg_cb(kconf, dr_msg_cb);
	/* Set the broker list */
	if (rd_kafka_conf_set(kconf, "bootstrap.servers", brokers, NULL, 0) != RD_KAFKA_CONF_OK) {
		fprintf(stderr, "Failed to set broker list\n");
		exit(1);
	}

	/* Create Kafka producer */
	rk = rd_kafka_new(RD_KAFKA_PRODUCER, kconf, NULL, 0);
	if (rk == NULL) {
		fprintf(stderr, "Failed to create Kafka producer\n");
		exit(1);
	}

	/* Create Kafka topic */
	rkt = rd_kafka_topic_new(rk, topic, NULL);
	if (rkt == NULL) {
		fprintf(stderr, "Failed to create Kafka topic\n");
		rd_kafka_destroy(rk);
		exit(1);
	}

	/* Produce message */
	err = rd_kafka_produce(rkt, RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_COPY, message, strlen(message), NULL, 0,
			       NULL);
	if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
		fprintf(stderr, "Failed to produce message: %s\n", rd_kafka_err2str(err));
		rd_kafka_topic_destroy(rkt);
		rd_kafka_destroy(rk);
		exit(1);
	}

	/* Wait for messages to be delivered */
	/* rd_kafka_flush(rk, 10000);  */

	if (err == RD_KAFKA_RESP_ERR_NO_ERROR) {
		while (status == NO_DELIVERY_REPORT) {

			status = rd_kafka_poll(rk, 1000);
			printf("in kafka module rd_kafka_poll() returned %d\n", status);
		}
		/*    err = status; */
	}

	/* Destroy Kafka topic and producer */

	rd_kafka_topic_destroy(rkt);
	rd_kafka_destroy(rk);

	return 0;
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
	mod_authenticate(rlm_rcode_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_kafka_thread_t *t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);
	(void)t;
	(void)p_result;

	printf("called kafka mod_authenticate()\n\n\n\n");
	send_kafka_message();

	return unlang_module_yield(request, mod_authenticate_resume, kafka_io_module_signal, ~FR_SIGNAL_CANCEL, NULL);
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
		.thread_instantiate 	= mod_thread_instantiate,
		.thread_detach      	= mod_thread_detach,
	},
	.method_group = {
		.bindings = (module_method_binding_t[]){
			{ .section = SECTION_NAME("authenticate", CF_IDENT_ANY), .method = mod_authenticate },
			MODULE_BINDING_TERMINATOR
		}
	}
};
