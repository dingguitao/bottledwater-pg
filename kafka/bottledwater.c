#include "connect.h"

#include <librdkafka/rdkafka.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_REPLICATION_SLOT "bottledwater"
#define APP_NAME "bottledwater"

/* The name of the logical decoding output plugin with which the replication
 * slot is created. This must match the name of the Postgres extension. */
#define OUTPUT_PLUGIN "bottledwater"

#define DEFAULT_BROKER_LIST "localhost:9092"

#define check(err, call) { err = call; if (err) return err; }

#define ensure(context, call) { \
    if (call) { \
        fprintf(stderr, "%s: %s\n", progname, (context)->client->error); \
        exit_nicely(context, 1); \
    } \
}

// TODO replace hard-coded value with call to schema registry:
//
// curl -X POST -i -H "Content-Type: application/vnd.schemaregistry.v1+json" \
//   --data '{"schema": "schema_as_string"}' \
//   http://localhost:8081/subjects/test-value/versions
//
// Returns: {"id":21}
//
// Message prefix is 5 bytes: one null "magic" byte, followed by the schema ID
// (as returned by the schema registry) as a 32-bit big-endian integer.
//
// https://github.com/confluentinc/schema-registry/blob/master/avro-serializer/src/main/java/io/confluent/kafka/serializers/AbstractKafkaAvroSerializer.java
#define MESSAGE_PREFIX "\0\0\0\0\x15"
#define MESSAGE_PREFIX_LEN 5

#define PRODUCER_CONTEXT_ERROR_LEN 512

typedef struct {
    client_context_t client;            /* The connection to Postgres */
    char *brokers;
    rd_kafka_conf_t *kafka_conf;
    rd_kafka_topic_conf_t *topic_conf;
    rd_kafka_t *kafka;
    rd_kafka_topic_t *topic; /* TODO need to support multiple topics */
    char error[PRODUCER_CONTEXT_ERROR_LEN];
} producer_context;

typedef producer_context *producer_context_t;

typedef struct {
    producer_context_t context;
    uint64_t wal_pos;
    Oid relid;
} msg_envelope;

typedef msg_envelope *msg_envelope_t;

static char *progname;

void usage(void);
void parse_options(producer_context_t context, int argc, char **argv);
char *parse_config_option(char *option);
void set_kafka_config(producer_context_t context, char *property, char *value);
void set_topic_config(producer_context_t context, char *property, char *value);
static int on_begin_txn(void *context, uint64_t wal_pos, uint32_t xid);
static int on_commit_txn(void *context, uint64_t wal_pos, uint32_t xid);
static int on_table_schema(void *context, uint64_t wal_pos, Oid relid, const char *schema_json,
        size_t schema_len);
static int on_insert_row(void *context, uint64_t wal_pos, Oid relid, const void *new_row_bin,
        size_t new_row_len, avro_value_t *new_row_val);
static int on_update_row(void *context, uint64_t wal_pos, Oid relid, const void *old_row_bin,
        size_t old_row_len, avro_value_t *old_row_val, const void *new_row_bin,
        size_t new_row_len, avro_value_t *new_row_val);
static int on_delete_row(void *context, uint64_t wal_pos, Oid relid, const void *old_row_bin,
        size_t old_row_len, avro_value_t *old_row_val);
static void on_deliver_msg(rd_kafka_t *kafka, const rd_kafka_message_t *msg, void *envelope);
void exit_nicely(producer_context_t context, int status);
client_context_t init_client(void);
producer_context_t init_producer(client_context_t client);
void start_producer(producer_context_t context);


void usage() {
    fprintf(stderr,
            "Exports a snapshot of a PostgreSQL database, followed by a stream of changes,\n"
            "and sends the data to a Kafka cluster.\n\n"
            "Usage:\n  %s [OPTION]...\n\nOptions:\n"
            "  -d, --postgres=postgres://user:pass@host:port/dbname   (required)\n"
            "                          Connection string or URI of the PostgreSQL server.\n"
            "  -s, --slot=slotname     Name of replication slot   (default: %s)\n"
            "                          The slot is automatically created on first use.\n"
            "  -b, --broker=host1[:port1],host2[:port2]...   (default: %s)\n"
            "                          Comma-separated list of Kafka broker hosts/ports.\n"
            "  -C, --kafka-config property=value\n"
            "                          Set global configuration property for Kafka producer\n"
            "                          (see --config-help for list of properties).\n"
            "  -T, --topic-config property=value\n"
            "                          Set topic configuration property for Kafka producer.\n"
            "  --config-help           Print the list of configuration properties. See also:\n"
            "            https://github.com/edenhill/librdkafka/blob/master/CONFIGURATION.md\n",
            progname, DEFAULT_REPLICATION_SLOT, DEFAULT_BROKER_LIST);
    exit(1);
}

/* Parse command-line options */
void parse_options(producer_context_t context, int argc, char **argv) {

    static struct option options[] = {
        {"postgres",     required_argument, NULL, 'd'},
        {"slot",         required_argument, NULL, 's'},
        {"broker",       required_argument, NULL, 'b'},
        {"kafka-config", required_argument, NULL, 'C'},
        {"topic-config", required_argument, NULL, 'T'},
        {"config-help",  no_argument,       NULL,  1 },
        {NULL,           0,                 NULL,  0 }
    };

    progname = argv[0];

    int option_index;
    while (true) {
        int c = getopt_long(argc, argv, "d:s:b:C:T:", options, &option_index);
        if (c == -1) break;

        switch (c) {
            case 'd':
                context->client->conninfo = strdup(optarg);
                break;
            case 's':
                context->client->repl.slot_name = strdup(optarg);
                break;
            case 'b':
                context->brokers = strdup(optarg);
                break;
            case 'C':
                set_kafka_config(context, optarg, parse_config_option(optarg));
                break;
            case 'T':
                set_topic_config(context, optarg, parse_config_option(optarg));
                break;
            case 1:
                rd_kafka_conf_properties_show(stderr);
                exit(1);
                break;
            default:
                usage();
        }
    }

    if (!context->client->conninfo || optind < argc) usage();
}

/* Splits an option string by equals sign. Modifies the option argument to be
 * only the part before the equals sign, and returns a pointer to the part after
 * the equals sign. */
char *parse_config_option(char *option) {
    char *equals = strchr(option, '=');
    if (!equals) {
        fprintf(stderr, "%s: Expected configuration in the form property=value, not \"%s\"\n",
                progname, option);
        exit(1);
    }

    // Overwrite equals sign with null, to split key and value into two strings
    *equals = '\0';
    return equals + 1;
}

void set_kafka_config(producer_context_t context, char *property, char *value) {
    if (rd_kafka_conf_set(context->kafka_conf, property, value,
                context->error, PRODUCER_CONTEXT_ERROR_LEN) != RD_KAFKA_CONF_OK) {
        fprintf(stderr, "%s: %s\n", progname, context->error);
        exit(1);
    }
}

void set_topic_config(producer_context_t context, char *property, char *value) {
    if (rd_kafka_topic_conf_set(context->topic_conf, property, value,
                context->error, PRODUCER_CONTEXT_ERROR_LEN) != RD_KAFKA_CONF_OK) {
        fprintf(stderr, "%s: %s\n", progname, context->error);
        exit(1);
    }
}

static int on_begin_txn(void *context, uint64_t wal_pos, uint32_t xid) {
    return 0;
}

static int on_commit_txn(void *context, uint64_t wal_pos, uint32_t xid) {
    return 0;
}

static int on_table_schema(void *context, uint64_t wal_pos, Oid relid, const char *schema_json,
        size_t schema_len) {
    return 0;
}

static int on_insert_row(void *context, uint64_t wal_pos, Oid relid, const void *new_row_bin,
        size_t new_row_len, avro_value_t *new_row_val) {
    int err = 0;
    const char *table_name = avro_schema_name(avro_value_get_schema(new_row_val));
    if (strcmp(table_name, "test") != 0) return 0; // TODO remove hard-coded topic

    msg_envelope_t envelope = malloc(sizeof(msg_envelope));
    memset(envelope, 0, sizeof(msg_envelope));
    envelope->context = context;
    envelope->wal_pos = wal_pos;
    envelope->relid = relid;

    char *msg = malloc(new_row_len + MESSAGE_PREFIX_LEN);
    memcpy(msg, MESSAGE_PREFIX, MESSAGE_PREFIX_LEN);
    memcpy(msg + MESSAGE_PREFIX_LEN, new_row_bin, new_row_len);

    err = rd_kafka_produce(((producer_context_t) context)->topic,
            RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_FREE,
            msg, new_row_len + MESSAGE_PREFIX_LEN, NULL, 0, envelope);

    // TODO apply backpressure if data from Postgres is coming in faster than we can send
    // it on to Kafka. Producer does not block, but signals a full buffer like this:
    // if (rd_kafka_errno2err(errno) == RD_KAFKA_RESP_ERR__QUEUE_FULL) ...

    if (err != 0) {
        fprintf(stderr, "%s: Failed to produce to Kafka: %s\n",
            progname, rd_kafka_err2str(rd_kafka_errno2err(errno)));
        return EIO;
    }

    return 0;
}

static int on_update_row(void *context, uint64_t wal_pos, Oid relid, const void *old_row_bin,
        size_t old_row_len, avro_value_t *old_row_val, const void *new_row_bin,
        size_t new_row_len, avro_value_t *new_row_val) {
    return 0;
}

static int on_delete_row(void *context, uint64_t wal_pos, Oid relid, const void *old_row_bin,
        size_t old_row_len, avro_value_t *old_row_val) {
    return 0;
}

/* Called by Kafka producer once per message sent, to report the delivery status
 * (whether success or failure). */
static void on_deliver_msg(rd_kafka_t *kafka, const rd_kafka_message_t *msg, void *opaque) {
    // The pointer that is the last argument to rd_kafka_produce is passed back
    // to us in the _private field in the struct. Seems a bit risky to rely on
    // a field called _private, but it seems to be the only way?
    msg_envelope_t envelope = (msg_envelope_t) msg->_private;

    if (msg->err) {
        fprintf(stderr, "Message delivery failed: %s\n",
            rd_kafka_message_errstr(msg));
    } else {
        uint64_t wal_pos = envelope->wal_pos;
        fprintf(stderr, "Message for WAL position %X/%X written to Kafka.\n",
                (uint32) (wal_pos >> 32), (uint32) wal_pos);
    }
    free(envelope);
}

/* Initializes the client context, which holds everything we need to know about
 * our connection to Postgres. */
client_context_t init_client() {
    frame_reader_t frame_reader = frame_reader_new();
    frame_reader->on_begin_txn    = on_begin_txn;
    frame_reader->on_commit_txn   = on_commit_txn;
    frame_reader->on_table_schema = on_table_schema;
    frame_reader->on_insert_row   = on_insert_row;
    frame_reader->on_update_row   = on_update_row;
    frame_reader->on_delete_row   = on_delete_row;

    client_context_t client = db_client_new();
    client->app_name = APP_NAME;
    client->repl.slot_name = DEFAULT_REPLICATION_SLOT;
    client->repl.output_plugin = OUTPUT_PLUGIN;
    client->repl.frame_reader = frame_reader;
    return client;
}

/* Initializes the producer context, which holds everything we need to know about
 * our connection to Kafka. */
producer_context_t init_producer(client_context_t client) {
    producer_context_t context = malloc(sizeof(producer_context));
    memset(context, 0, sizeof(producer_context));
    client->repl.frame_reader->cb_context = context;

    context->client = client;
    context->brokers = DEFAULT_BROKER_LIST;
    context->kafka_conf = rd_kafka_conf_new();
    context->topic_conf = rd_kafka_topic_conf_new();

    set_topic_config(context, "produce.offset.report", "true");
    rd_kafka_conf_set_dr_msg_cb(context->kafka_conf, on_deliver_msg);
    return context;
}

/* Connects to Kafka. This should be done before connecting to Postgres, as it
 * simply calls exit(1) on failure. */
void start_producer(producer_context_t context) {
    context->kafka = rd_kafka_new(RD_KAFKA_PRODUCER, context->kafka_conf,
            context->error, PRODUCER_CONTEXT_ERROR_LEN);
    if (!context->kafka) {
        fprintf(stderr, "%s: Could not create Kafka producer: %s\n", progname, context->error);
        exit(1);
    }

    if (rd_kafka_brokers_add(context->kafka, context->brokers) == 0) {
        fprintf(stderr, "%s: No valid Kafka brokers specified\n", progname);
        exit(1);
    }

    /* TODO fix hard-coded topic name. Use rd_kafka_topic_conf_dup(). */
    char *topicname = "test3";
    context->topic = rd_kafka_topic_new(context->kafka, topicname, context->topic_conf);
    if (!context->topic) {
        fprintf(stderr, "%s: Cannot open Kafka topic %s: %s\n", progname, topicname,
                rd_kafka_err2str(rd_kafka_errno2err(errno)));
        exit(1);
    }
}

void exit_nicely(producer_context_t context, int status) {
    frame_reader_free(context->client->repl.frame_reader);
    db_client_free(context->client);
    // TODO shut down Kafka producer gracefully
    exit(status);
}


int main(int argc, char **argv) {
    producer_context_t context = init_producer(init_client());

    parse_options(context, argc, argv);
    start_producer(context);
    ensure(context, db_client_start(context->client));

    bool snapshot = false;
    replication_stream_t stream = &context->client->repl;

    if (context->client->sql_conn) {
        fprintf(stderr, "Created replication slot \"%s\", capturing consistent snapshot \"%s\".\n",
                stream->slot_name, stream->snapshot_name);
        snapshot = true;
    } else {
        fprintf(stderr, "Replication slot \"%s\" exists, streaming changes from %X/%X.\n",
                stream->slot_name,
                (uint32) (stream->start_lsn >> 32), (uint32) stream->start_lsn);
    }

    while (context->client->status >= 0) { /* TODO install signal handler for graceful shutdown */
        ensure(context, db_client_poll(context->client));

        if (snapshot && !context->client->sql_conn) {
            snapshot = false;
            fprintf(stderr, "Snapshot complete, streaming changes from %X/%X.\n",
                    (uint32) (stream->start_lsn >> 32), (uint32) stream->start_lsn);
        }

        if (context->client->status == 0) {
            ensure(context, db_client_wait(context->client));
        }

        rd_kafka_poll(context->kafka, 0);
    }

    exit_nicely(context, 0);
    return 0;
}
