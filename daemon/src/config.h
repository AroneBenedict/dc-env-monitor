/* Configuration file: a small INI dialect.
 *
 *   [device]
 *   id = dc1-row3
 *   collector_host = 127.0.0.1
 *   collector_port = 7070
 *   spool_dir = /var/lib/envmond
 *   report_interval = 60
 *   stale_after = 180
 *
 *   [sensor rack1-inlet]
 *   kind = temperature        ; temperature | humidity | door | leak
 *   warn_high = 27
 *   crit_high = 32
 *   hysteresis = 1
 *   delay = 120
 */
#ifndef ENVMOND_CONFIG_H
#define ENVMOND_CONFIG_H

#include "common.h"

enum sensor_kind { KIND_TEMPERATURE, KIND_HUMIDITY, KIND_DOOR, KIND_LEAK };

struct sensor_config {
    char id[MAX_ID];
    enum sensor_kind kind;
    double warn_low, crit_low, warn_high, crit_high; /* NAN when not set */
    double hysteresis;
    long delay; /* seconds a condition must hold before the alarm state changes */
    double min_valid, max_valid; /* readings outside this range are rejected as sensor faults */
    double max_step;             /* a jump bigger than this from the last accepted value is a glitch (0 = off) */
};

struct config {
    char device_id[MAX_ID];
    char collector_host[64];
    int collector_port;
    char spool_dir[MAX_PATH];
    long report_interval;
    long stale_after;
    long spool_max_bytes;
    int nsensors;
    struct sensor_config sensors[MAX_SENSORS];
};

/* Parse configuration text. On error returns -1 and writes "line N: reason" into err. */
int config_parse(const char *text, struct config *out, char *err, size_t errlen);

/* Read and parse a file. */
int config_load(const char *path, struct config *out, char *err, size_t errlen);

/* Index of a sensor by id, or -1. */
int config_find(const struct config *cfg, const char *id);

const char *kind_name(enum sensor_kind kind);

#endif
