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

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/module_rlm.h>
#include <freeradius-devel/unlang/xlat_func.h>

#include <librdkafka/rdkafka.h>

#include <fcntl.h>

static fr_sbuff_parse_rules_t const header_arg_parse_rules = {
	.terminals = &FR_SBUFF_TERMS(
		L("\t"),
		L(" "),
		L("!")
	)
};

#define LOG_PREFIX inst->name

#define RLM_KAFKA_PROP_SET(CONF, PROP, VALUE, BUF_ERRSTR)								\
	do {														\
		if (rd_kafka_conf_set(CONF, PROP, VALUE, BUF_ERRSTR, sizeof(BUF_ERRSTR)) != RD_KAFKA_CONF_OK )		\
			ERROR("Error setting Kafka global property: '%s=%s' : %s\n", PROP, VALUE, BUF_ERRSTR);		\
	} while (0)

#define RLM_KAFKA_TOPIC_PROP_SET(CONF, PROP, VALUE, BUF_ERRSTR)								\
	do {														\
		if (rd_kafka_topic_conf_set(CONF, PROP, VALUE, BUF_ERRSTR, sizeof(BUF_ERRSTR)) != RD_KAFKA_CONF_OK )	\
			ERROR("Error setting Kafka topic property: '%s=%s' : %s\n", PROP, VALUE, BUF_ERRSTR);		\
	} while (0)

typedef struct rlm_kafka_rkt_by_name {

	fr_rb_node_t		node;

	const char		*name;
	rd_kafka_topic_t	*rkt;

	/*
	 *  Only one entry is the "owner" for a topic, and all others are
	 *  references to it (having ref = true)
	 */
	bool 			ref;

} rkt_by_name_entry_t;

typedef struct rlm_kafka_t {

	char const *name;

	bool async;

	char const *bootstrap;
	char const *schema;

	char const *stats_filename;
	FILE *stats_file;

	rd_kafka_t *rk;

	fr_rb_tree_t *rkt_by_name_tree;

} rlm_kafka_t;

typedef struct {

	rlm_kafka_t		*inst;
	rd_kafka_queue_t	*queue;         //!< Per-thread event queue
	int			event_fd;       //!< Read end of socketpair
	int			write_fd;       //!< Write end for librdkafka
	uint64_t		payload;

} rlm_kafka_thread_t;

typedef struct {
	int             result;             //!< Delivery result (0 = success)
	char const      *error_msg;         //!< Error message if failed
} rlm_kafka_rctx_t;

typedef struct {
	rd_kafka_queue_t    *queue;		//!< Queue to write notification to
	request_t           *request;		//!< Passed to unlang_interpret_mark_runnable()
	bool                active;		//!< Set to false if request is cleaned up
	rlm_kafka_rctx_t    *rctx;		//!< Where we write notification reports
} kafka_rctx_t;


static const conf_parser_t global_config[] = {
	CONF_PARSER_TERMINATOR
};

static const conf_parser_t topic_config[] = {
	CONF_PARSER_TERMINATOR
};

static const conf_parser_t stats_config[] = {
	{ FR_CONF_OFFSET_FLAGS("file", CONF_FLAG_FILE_OUTPUT, rlm_kafka_t, stats_filename) },
	CONF_PARSER_TERMINATOR
};

/*
 *  It would be nice to have a/synchronous delivery be a property set for each
 *  topic, but unfortunately this is not possible.
 *
 *  High-throughput asynchronous requires a sufficiently large linger.ms to
 *  ensure batched message delivery.
 *
 *  Synchronous delivery requires linger.ms = 0 to avoid unnecessary delays.
 *
 *  However, linger.ms is a global property, and rd_kafka_flush() purges the
 *  queue of all topics.
 *
 */
static const conf_parser_t module_config[] = {
	{ FR_CONF_OFFSET_FLAGS("bootstrap-servers", CONF_FLAG_REQUIRED, rlm_kafka_t, bootstrap) },
	{ FR_CONF_OFFSET("asynchronous", rlm_kafka_t, async), .dflt = "no" },
	{ FR_CONF_POINTER("global-config", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = (void const*) global_config },
	{ FR_CONF_POINTER("topic-config", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = (void const*) topic_config },
	{ FR_CONF_POINTER("statistics", 0, CONF_FLAG_SUBSECTION, NULL), .subcs = (void const*) stats_config },
	CONF_PARSER_TERMINATOR
};

static int stats_cb (UNUSED rd_kafka_t *rk, char *json, size_t json_len, void *opaque) {
	rlm_kafka_t *inst = opaque;

	/*
	 *  Apparently this callback does not need to be re-entrant...
	 */
	DEBUG3("stats callback");
	fprintf(inst->stats_file, "%.*s\n", (int)json_len, json);
	fflush(inst->stats_file);

	return 0;
}

static void dr_msg_cb(UNUSED rd_kafka_t *rk, const rd_kafka_message_t *rkmessage, UNUSED void *opaque)
{
	int32_t			broker_id = -1;
	const char		*persisted = "unknown";

	kafka_rctx_t		*krctx;


	DEBUG3("XXXXXXXXXXXXXXXXXXX dr_msg_cb CALLED");
	/*
	 *  TODO turn into a "decoupled" mode, where the module usually returns OK without yielding
	 *
	 */
	/*
	if (rkmessage->_private) {
		status	= (rd_kafka_resp_err_t *)rkmessage->_private;
		*status = rkmessage->err;
		return;
	}
	*/

	if (!rkmessage->_private) {
		ERROR("rlm_kafka: Delivery report received with NULL opaque data");
		return;
	}

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

	DEBUG3("Kafka delivery report '%s' for key: %.*s (%s persisted to broker %" PRId32 ")\n",
	      rd_kafka_err2str(rkmessage->err),
	      (int)rkmessage->len,
	      (char *)rkmessage->payload,
	      persisted,
	      broker_id);

	krctx = talloc_get_type_abort(rkmessage->_private, kafka_rctx_t);

	/*
	*  Check if request is still active
	*/
	if (!krctx->active) {
		WARN("rlm_kafka: Delivery report received for inactive request, discarding");
		talloc_free(krctx);
		return;
	}

	/*
	 *  TODO
	 *
	 *  rcode based on !rkmessage->err
	 *  Set Module-Failure-Message of fail
	 *  Set attrs for broker_id and persisted?
	 *
	 */

	/*
	 *  Update the result context with delivery status
	 *  TODO not necessarily needed
	 */
	if (rkmessage->err) {
		krctx->rctx->result = -1;
		krctx->rctx->error_msg = rd_kafka_err2str(rkmessage->err);
	} else {
		krctx->rctx->result = 0;
		krctx->rctx->error_msg = NULL;
	}

	/*
	 *  Mark request as runnable to resume execution
	 */
	unlang_interpret_mark_runnable(krctx->request);

}


/*
 * This callback may be triggered spontaneously from any thread at any time.
 *
 */
static void log_cb(UNUSED const rd_kafka_t *rk, int level, UNUSED const char *facility, const char *buf)
{

	/*
	 *  Map Kafka error levels (based on syslog severities) to FR log levels
	 */
	switch (level) {
	case 4:
		WARN("%s", buf);
		break;
	case 5:
	case 6:
		INFO("%s", buf);
		break;
	case 7:
		DEBUG("%s", buf);
		break;
	default:
		ERROR("%s", buf);
	}
}


/*
 *  Note: No error_cb function: "If no error_cb is registered ... then the
 *  errors will be logged [log_cb] instead."
 *
 */


static void kafka_io_xlat_signal(xlat_ctx_t const *xctx, UNUSED request_t *request, UNUSED fr_signal_t action)
{

	kafka_rctx_t *krctx = talloc_get_type_abort(xctx->rctx, kafka_rctx_t);

	switch (action) {
	case FR_SIGNAL_CANCEL:
		/*
		 *  Mark the kafka context as inactive so delivery reports are discarded
		 */
		krctx->active = false;
		break;

	default:
		break;
	}

}

static xlat_action_t kafka_xlat_resume(TALLOC_CTX *ctx, fr_dcursor_t *out,
                              xlat_ctx_t const *xctx,
                              UNUSED request_t *request, UNUSED fr_value_box_list_t *in)
{

	kafka_rctx_t		*krctx = talloc_get_type_abort(xctx->rctx, kafka_rctx_t);
//        rlm_kafka_thread_t	*t = talloc_get_type_abort(xctx->mctx->thread, rlm_kafka_thread_t);
        fr_value_box_t		*vb;

        MEM(vb = fr_value_box_alloc(ctx, FR_TYPE_BOOL, NULL));
        vb->vb_bool = krctx->rctx->result;

	// TODO What happens to lifecycle of krctx and rctx if callback never fires?
	talloc_free(krctx);

        fr_dcursor_insert(out, vb);

        return XLAT_ACTION_DONE;

}


static int create_headers(request_t *request, const char *in, rd_kafka_headers_t **out)
{
	rd_kafka_headers_t	*hdrs = NULL;
	fr_pair_list_t		header_vps, vps;
	int			num_vps;
	fr_sbuff_t		sbuff;

	if (!in)
		return 0;

	fr_pair_list_init(&header_vps);
	fr_pair_list_init(&vps);

	sbuff = FR_SBUFF_IN(in, strlen(in));
	fr_sbuff_adv_past_whitespace(&sbuff, SIZE_MAX, NULL);

	while (fr_sbuff_extend(&sbuff)) {
		bool		negate = false;
		tmpl_t	  *vpt = NULL;
		ssize_t	 slen;

		/* Check if we should be removing attributes */
		if (fr_sbuff_next_if_char(&sbuff, '!'))
			negate = true;

		/* Decode next attr template */
		slen = tmpl_afrom_attr_substr(request, NULL, &vpt,
						&sbuff,
						&header_arg_parse_rules,
						&(tmpl_rules_t){
							.attr = {
								.list_def = request_attr_request,
								.allow_wildcard = true,
								.dict_def = request->proto_dict,
							}
						});
		if (slen <= 0) {
			fr_sbuff_set(&sbuff, (size_t)(slen * -1));
			REMARKER(fr_sbuff_start(&sbuff), fr_sbuff_used(&sbuff), "%s", fr_strerror());
		error:
			fr_pair_list_free(&header_vps);
			talloc_free(vpt);
			return -1;
		}

		/*
		 * Get attributes from the template.
		 * Missing attribute isn't an error (so -1, not 0).
		 */
		if (tmpl_copy_pairs(request, &vps, request, vpt) < -1) {
			RPEDEBUG("Error copying attributes");
			goto error;
		}

		if (negate) {
			fr_pair_t *vp;

			/* Remove all template attributes from JSON list */
			for (vp = fr_pair_list_head(&vps);
				vp;
				vp = fr_pair_list_next(&vps, vp)) {

				fr_pair_t *vpm = fr_pair_list_head(&header_vps);
				while (vpm) {
					if (vp->da == vpm->da) {
						fr_pair_t *next = fr_pair_list_next(&header_vps, vpm);
						fr_pair_delete(&header_vps, vpm);
						vpm = next;
						continue;
					}
					vpm = fr_pair_list_next(&header_vps, vpm);
				}
			}

			fr_pair_list_free(&vps);
		} else {
			/* Add template VPs to JSON list */
			fr_pair_list_append(&header_vps, &vps);
		}

		TALLOC_FREE(vpt);

		/* Jump forward to next attr */
		fr_sbuff_adv_past_whitespace(&sbuff, SIZE_MAX, NULL);
	}

	/*
	 *  Create the Kafka headers for the derived VPs
	 *
	 */
	num_vps = fr_pair_list_num_elements(&header_vps);
	if (num_vps == 0)
		return 0;

	hdrs = rd_kafka_headers_new(num_vps);
	fr_pair_list_foreach(&header_vps, vp) {
		rd_kafka_resp_err_t err;
		char *value;
		int len;

		len = fr_value_box_aprint(NULL, &value, &vp->data, NULL);
		err = rd_kafka_header_add(hdrs, vp->da->name, vp->da->name_len, value, len);
		talloc_free(value);
		if (err) {
			rd_kafka_headers_destroy(hdrs);
			return -1;
		}
	}

	*out = hdrs;
	return 0;

}

/*
 *  Format is one of the following:
 *
 *  - %kafka("<topic>", "<key>", "<message>", "<headers>")
 *
 */
static xlat_action_t kafka_xlat(UNUSED TALLOC_CTX *ctx, UNUSED fr_dcursor_t *out,
	UNUSED xlat_ctx_t const *xctx, UNUSED request_t *request,
	fr_value_box_list_t *in)
{

	rlm_kafka_t const	*inst = talloc_get_type_abort_const(xctx->mctx->mi->data, rlm_kafka_t);
	rlm_kafka_thread_t	*t = talloc_get_type_abort(xctx->mctx->thread, rlm_kafka_thread_t);

	rkt_by_name_entry_t	*entry, my_topic;
	rd_kafka_headers_t	*hdrs = NULL;
	kafka_rctx_t		*krctx;
	rlm_kafka_rctx_t	*rctx;
	rd_kafka_resp_err_t	err;

	fr_value_box_t		*topic_vb;
	fr_value_box_t		*key_vb;
	fr_value_box_t		*msg_vb;
	fr_value_box_t		*headers_vb;

	XLAT_ARGS(in, &topic_vb, &key_vb, &msg_vb, &headers_vb);

	if (topic_vb->vb_length == 0) {
		REDEBUG("Kafka topic may not be empty");
		return XLAT_ACTION_FAIL;
	}

	my_topic.name = topic_vb->vb_strvalue;
	entry = fr_rb_find(inst->rkt_by_name_tree, &my_topic);
	if (!entry || !entry->rkt) {
		REDEBUG("No configuration section exists for kafka topic \"%s\"", topic_vb->vb_strvalue);
		return XLAT_ACTION_FAIL;
	}

	if (headers_vb && create_headers(request, headers_vb->vb_strvalue, &hdrs) < 0) {
		REDEBUG("Failed to create headers");
		return XLAT_ACTION_FAIL;
	}

	/*
	 *  Allocate opaque context in NULL talloc context
	 */
	krctx = talloc_zero(NULL, kafka_rctx_t);
	if (!krctx) {
		RERROR("rlm_kafka: Failed to allocate opaque context");
		return XLAT_ACTION_FAIL;
	}

	/*
	*  Allocate resume context for results
	*/
	rctx = talloc_zero(krctx, rlm_kafka_rctx_t);
	if (!rctx) {
		ERROR("rlm_kafka: Failed to allocate request context");
		talloc_free(krctx);
		return XLAT_ACTION_FAIL;
	}

	/*
	*  Initialize opaque context
	*/
	krctx->queue = t->queue;
	krctx->request = request;
	krctx->active = true;
	krctx->rctx = rctx;

	RDEBUG3("Producing %ld byte message with key=%.*s\n", msg_vb->vb_length,
		(int)key_vb->vb_length, key_vb->vb_strvalue);

	err = rd_kafka_producev(
		inst->rk,
		RD_KAFKA_V_RKT(entry->rkt),
		RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
		RD_KAFKA_V_KEY(UNCONST(char *const, key_vb->vb_strvalue), key_vb->vb_length),
		RD_KAFKA_V_VALUE(UNCONST(char *const, msg_vb->vb_strvalue), msg_vb->vb_length),
		RD_KAFKA_V_HEADERS(hdrs),
		RD_KAFKA_V_OPAQUE(krctx),  // TODO detached mode
		RD_KAFKA_V_END
				);

	/*
	 *  If rd_kafka_producev() failed then we still own any headers.
	 *
	 */
	if (err != RD_KAFKA_RESP_ERR_NO_ERROR && hdrs)
		rd_kafka_headers_destroy(hdrs);

	if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
		RERROR("Failed to produce to Kafka topic: %s: %s\n",
			rd_kafka_topic_name(entry->rkt), rd_kafka_err2str(err));
		return XLAT_ACTION_FAIL;
	}

	// TODO Return XLAT_ACTION_DONE in detached mode
	return unlang_xlat_yield(request, kafka_xlat_resume, kafka_io_xlat_signal, ~FR_SIGNAL_CANCEL, krctx);

}

static int8_t rkt_by_name_cmp(void const *one, void const *two)
{
	rkt_by_name_entry_t const *a = (rkt_by_name_entry_t const *) one;
	rkt_by_name_entry_t const *b = (rkt_by_name_entry_t const *) two;

	return CMP(strcmp(a->name, b->name), 0);
}

static int destruct_entry(rkt_by_name_entry_t *entry) {
	/*
	 *  Destroy rkt only if we are the owner (not a reference)
	 *
	 */
	if (!entry->ref && entry->rkt)
		rd_kafka_topic_destroy(entry->rkt);
	entry->rkt = NULL;
	return 0;
}

static void rkt_by_name_free(void *data)
{
	rkt_by_name_entry_t *entry = (rkt_by_name_entry_t *) data;
	talloc_free(entry);
}

static int instantiate_topic(CONF_SECTION *cs, rlm_kafka_t *inst, char *errstr) {

	CONF_PAIR		*cp;
	rd_kafka_topic_conf_t	*tconf;
	rd_kafka_topic_t	*rkt;
	bool			ref = false;
	rkt_by_name_entry_t	*entry = NULL;
	const char		*name = cf_section_name2(cs);

	/*
	 *  Short circuit for when we are given a reference to an existing topic
	 *
	 */
	cp = cf_pair_find_next(cs, NULL, NULL);
	if (cp) {
		char const *attr = cf_pair_attr(cp);
		char const *value = cf_pair_value(cp);

		if (strcmp(attr, "reference") == 0) {
			rkt_by_name_entry_t my_topic;

			my_topic.name = value;
			entry = fr_rb_find(inst->rkt_by_name_tree, &my_topic);
			if (!entry || !entry->rkt) {
				ERROR("Couldn't reference Kafka topic \"%s\" for \"%s\"",
				      value, cf_section_name2(cs));
				return -1;
			}
			if (cf_pair_find_next(cs, cp, NULL)) {
				ERROR("A reference for another Kafka topic must be the only attribute");
				return -1;
			}
			DEBUG3("Kafka topic \"%s\" configured as a reference to \"%s\"",
			       cf_section_name2(cs), value);
			rkt = entry->rkt;
			ref = true;
			goto finalise;
		}
	}

	/*
	 *  Configuration for the new topic
	 *
	 */
	tconf = rd_kafka_topic_conf_new();

	/*
	 *  When synchronous, don't block for longer than a typical request timeout.
	 *
	 *  Can be overridden by message.timeout.ms in the topic conf_section
	 */
	if (!inst->async)
		RLM_KAFKA_TOPIC_PROP_SET(tconf, "message.timeout.ms", "30000", errstr);

	/*
	 *  Set topic properties from the topic conf_section
	 */
	cp = NULL;
	do {

		cp = cf_pair_find_next(cs, cp, NULL);
		if (cp) {
			char const *attr = cf_pair_attr(cp);
			char const *value = cf_pair_value(cp);
			if (strcmp(attr, "name") == 0) {  /* Override section name */
				name = value;
				continue;
			} else if (strcmp(attr, "reference") == 0) {
				ERROR("A reference for another Kafka topic must be the only attribute");
				rd_kafka_topic_conf_destroy(tconf);
				return -1;
			}
			RLM_KAFKA_TOPIC_PROP_SET(tconf, attr, value, errstr);
		}
	} while (cp != NULL);

	/*
	 *  Show the topic configurations for debugging
	 */
	if (fr_debug_lvl >= L_DBG_LVL_3) {
		size_t		cnt, i;
		const char	**arr;

		DEBUG3("Configuration for Kafka topic \"%s\":", name);
		for (i = 0, arr = rd_kafka_topic_conf_dump(tconf, &cnt); i < cnt; i += 2)
			DEBUG3("\t%s = %s", arr[i], arr[i + 1]);
	}

	/*
	 *  And create the topic according to the configuration.
	 *
	 *  Upon success, the rkt assumes responsibility for tconf
	 *
	 */
	rkt = rd_kafka_topic_new(inst->rk, name, tconf);
	if (!rkt) {
		ERROR("Failed to create Kafka topic \"%s\"", name);
		rd_kafka_topic_conf_destroy(tconf);
		return -1;
	}

finalise:

	/*
	 *  Finally insert the entry into the rbtree.
	 *
	 */
	entry = talloc(NULL, rkt_by_name_entry_t);
	if (!entry)
		return -1;
	talloc_set_destructor(entry, destruct_entry);
	entry->name = talloc_strdup(entry, cf_section_name2(cs));
	if (!entry->name) {
	fail:
		talloc_free(entry);
		return -1;
	}
	entry->rkt = rkt;
	entry->ref = ref;

	if (!fr_rb_insert(inst->rkt_by_name_tree, entry))
		goto fail;

	DEBUG("Created Kafka topic for \"%s\"", name);

	return 0;

}

static void _kafka_pipe_read(UNUSED fr_event_list_t *el, int fd, UNUSED int flags, void *uctx)
{
    rlm_kafka_thread_t	*t = talloc_get_type_abort(uctx, rlm_kafka_thread_t);
    rd_kafka_event_t	*event;
    char		buffer[256];

    /*
     *  Clear the notification by reading from socketpair
     *  TODO cleaner way?
     */
    while (read(fd, buffer, sizeof(buffer)) > 0) {
        /* Keep reading until empty */
    }

    /*
     *  Process all available events
     */
    while ((event = rd_kafka_queue_poll(t->queue, 0)) != NULL) {
        switch (rd_kafka_event_type(event)) {
        case RD_KAFKA_EVENT_DR:
	    /*
             *  Delivery report event - the actual delivery report handling
             *  was already done by the kafka_delivery_cb() callback when
             *  this event was created. We just need to clean up the event.
             */
            break;

/*
        case RD_KAFKA_EVENT_ERROR:
            ERROR("rlm_kafka: Event error: %s", rd_kafka_event_error_string(event));
            break;

        case RD_KAFKA_EVENT_LOG:
            DEBUG3("rlm_kafka: %s", rd_kafka_event_log(event)->str);
            break;
*/

        default:
            WARN("rlm_kafka: Received unhandled event type: %d", rd_kafka_event_type(event));
            break;
        }

        rd_kafka_event_destroy(event);
    }
}

static int mod_bootstrap(module_inst_ctx_t const *mctx)
{

	xlat_t *xlat;

	static xlat_arg_parser_t const kafka_xlat_args[] = {
		{ .required = true, .single = true, .type = FR_TYPE_STRING },	/* Topic */
		{ .required = true, .concat = true, .type = FR_TYPE_STRING },	/* Key */
		{ .required = true, .concat = true, .type = FR_TYPE_STRING },	/* Message */
		{ .concat = true, .type = FR_TYPE_STRING },			/* Headers */
		XLAT_ARG_PARSER_TERMINATOR
	};

	xlat = module_rlm_xlat_register(mctx->mi->boot, mctx, NULL, kafka_xlat, FR_TYPE_SIZE);
	xlat_func_args_set(xlat, kafka_xlat_args);

	return 0;

}

static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_kafka_t		*inst = talloc_get_type_abort(mctx->mi->data,rlm_kafka_t);
	CONF_SECTION 		*conf = mctx->mi->conf;
	rd_kafka_conf_t		*kconf;
	char			errstr[512];
	CONF_PAIR		*cp = NULL;
	CONF_SECTION		*cs = NULL;

	inst->name = mctx->mi->name;

	/*
	 *  Configuration for the global producer
	 */
	kconf = rd_kafka_conf_new();

	rd_kafka_conf_set_opaque(kconf, inst);

	DEBUG3("Registering Kafka logging callback");
	rd_kafka_conf_set_log_cb(kconf, log_cb);

	DEBUG3("Registering Kafka delivery report callback");
	rd_kafka_conf_set_dr_msg_cb(kconf, dr_msg_cb);

	if (inst->stats_filename) {
		DEBUG3("Opening Kafka statistics file for writing: %s", inst->stats_filename);
		inst->stats_file = fopen(inst->stats_filename, "a");
		if (!inst->stats_file) {
			ERROR("Error opening Kafka statistics file: %s", inst->stats_filename);
			/* Carry on, just don't log stats */
		} else {
			DEBUG3("Registering Kafka statistics callback");
			rd_kafka_conf_set_stats_cb(kconf, stats_cb);
		}
	}

	RLM_KAFKA_PROP_SET(kconf, "bootstrap.servers", inst->bootstrap, errstr);

	/*
	 *  Set global properties from the global conf_section
	 */
	do {
		CONF_SECTION *gc = cf_section_find(conf, "global-config", NULL);

		cp = cf_pair_find_next(gc, cp, NULL);
		if (cp) {
			char const *attr, *value;
			attr = cf_pair_attr(cp);
			value = cf_pair_value(cp);
			RLM_KAFKA_PROP_SET(kconf, attr, value, errstr);
		}
	} while (cp != NULL);

	/*
	 *  When configured to send synchronously, avoid plugging the requests
	 *  since we are not batching and desire immediate responses.
	 *
	 *  Overrides and linger.ms that is set in the global conf_section.
	 */
	if (!inst->async)
		RLM_KAFKA_PROP_SET(kconf, "linger.ms", "0", errstr);

	/*
	 *  Show the global configuration for debugging
	 */
	if (fr_debug_lvl >= L_DBG_LVL_3) {
		size_t		cnt, i;
		const char	**arr;

		DEBUG3("Kafka global configuration:");
		for (i = 0, arr = rd_kafka_conf_dump(kconf, &cnt); i < cnt; i += 2)
			DEBUG3("\t%s = %s", arr[i], arr[i + 1]);
	}

	/*
	 *  And create the producer according to the configuration, which sets
	 *  up a separate handler ("rdk:main") thread and a set of
	 *  "rdk:brokerN" threads, one per broker.
	 *
	 *  librdkafka attempts a lot of blunt (unconfigurable), global
	 *  initialisation of dependent libraries here:
	 *
	 *    - cJSON library has it's allocation functions overridden, but
	 *      just to wrappers around malloc / realloc / free, etc. so this
	 *      is harmless.
	 *    - An attempt is made to initialise cURL, however the cURL library
	 *      maintains a reference count that prevents duplicate
	 *      reinitialisation.
	 *    - Cyrus SASL is similarly reference counted.
	 *    - For OpenSSL < 1.1.0 there is an attempted reinitialisation that
	 *      would clobber settings so at build time we enforce a minimum
	 *      version that no longer requires global initialisation.
	 *
	 *  There may still be unknown cases where other module's configuration
	 *  is trampled on, so best to test overall server functionality
	 *  carefully when enabling this module.
	 *
	 */
	inst->rk = rd_kafka_new(RD_KAFKA_PRODUCER, kconf, errstr, sizeof(errstr));
	if (!inst->rk) {
		ERROR("Failed to create new Kafka producer: %s\n", errstr);
		rd_kafka_conf_destroy(kconf);
		return -1;
	}

	/*
	 *  Instantiate a topic for each named topic-config section
	 *
	 */
	inst->rkt_by_name_tree = fr_rb_inline_talloc_alloc(inst, rkt_by_name_entry_t, node, rkt_by_name_cmp, rkt_by_name_free);
	if (!inst->rkt_by_name_tree) return -1;

	while ((cs = cf_section_find_next(conf, cs, "topic-config", CF_IDENT_ANY))) {

		if (!cf_section_name2(cs))
			continue;

		if (instantiate_topic(cs, inst, errstr) != 0) {
			ERROR("Failed to instantiate new Kafka topic for %s\n",
			      cf_section_name2(cs));
			talloc_free(inst->rkt_by_name_tree);
			rd_kafka_destroy(inst->rk);
			return -1;
		}

	}

	return 0;
}

static int mod_detach(module_detach_ctx_t const *mctx)
{
	rlm_kafka_t		*inst = talloc_get_type_abort(mctx->mi->data, rlm_kafka_t);
	rd_kafka_resp_err_t	err;

	if (inst->stats_file) {
		DEBUG3("Closing Kafka statistics file");
		fclose(inst->stats_file);
	}

	DEBUG3("Flushing Kafka queues");
	if ((err = rd_kafka_flush(inst->rk, 10*1000)) == RD_KAFKA_RESP_ERR__TIMED_OUT)
		ERROR("Flush failed: %s\n", rd_kafka_err2str(err));

	talloc_free(inst->rkt_by_name_tree);

	rd_kafka_destroy(inst->rk);

	return 0;
}


static int mod_thread_instantiate(module_thread_inst_ctx_t const *mctx)
{
	rlm_kafka_t		*inst = talloc_get_type_abort(mctx->mi->data, rlm_kafka_t);
	rlm_kafka_thread_t	*t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);
	int			sockfd[2] = { -1, -1 };
	int			flags;

	t->inst = inst;
	t->payload = 1;

	/*
	*  Kafka event queue per thread
	*/
	t->queue = rd_kafka_queue_new(inst->rk);
	if (!t->queue) {
		ERROR("rlm_kafka: Failed to create event queue");
error:
		if (sockfd[0] >= 0) close(sockfd[0]);
		if (sockfd[1] >= 0) close(sockfd[1]);
		if (t->queue) rd_kafka_queue_destroy(t->queue);
		return -1;
	}

	/*
	*  Create socketpair for event notifications
	*/
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockfd) < 0) {
		ERROR("Failed to create socketpair: %s", fr_syserror(errno));
		goto error;
	}

	fr_assert((sockfd[0] >= 0) && (sockfd[1] >= 0));

	t->event_fd = sockfd[0];  /* Read end */
	t->write_fd = sockfd[1];  /* Write end */

	/*
	 *  Make read end non-blocking
	 *  TODO check if needed
	 */
	flags = fcntl(t->event_fd, F_GETFL);
	if (flags <0) {
		ERROR("Failed getting socket flags whilst setting O_NONBLOCK");
		goto error;
	}
	flags |= O_NONBLOCK;
        if (fcntl(t->event_fd, F_SETFL, flags) < 0) {
		ERROR("rlm_kafka: Failed to set socket non-blocking: %s", fr_syserror(errno));
		goto error;
        }

	/*
	 *  Register read end with event loop
	 *  TODO do we need _kafka_pipe_error
	 *  TODO check ctx argument
	 */
	if (fr_event_fd_insert(mctx->thread, NULL, mctx->el, t->event_fd,
			       _kafka_pipe_read, NULL, NULL, t) < 0) {
		ERROR("rlm_kafka: Failed to register event handler");
		goto error;
	}

	// Hack - perhaps DR are only sent to the main queue?
	rd_kafka_queue_forward(rd_kafka_queue_get_main(inst->rk), t->queue);


	/*
	*  Enable I/O event notifications on the queue
	*/
	rd_kafka_queue_io_event_enable(t->queue, t->write_fd, &t->payload, sizeof(t->payload));

	return 0;
}

static int mod_thread_detach(module_thread_inst_ctx_t const *mctx)
{
	rlm_kafka_thread_t *t = talloc_get_type_abort(mctx->thread, rlm_kafka_thread_t);

	if (t->event_fd >= 0) close(t->event_fd);
	if (t->write_fd >= 0) close(t->write_fd);
	rd_kafka_queue_destroy(t->queue);

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
		.magic			= MODULE_MAGIC_INIT,
		.name			= "kafka",
		.inst_size		= sizeof(rlm_kafka_t),
		.config			= module_config,
		.bootstrap		= mod_bootstrap,
		.instantiate		= mod_instantiate,
		.detach			= mod_detach,
		.thread_inst_size	= sizeof(rlm_kafka_thread_t),
		.thread_instantiate	= mod_thread_instantiate,
		.thread_detach		= mod_thread_detach,
	}
};
