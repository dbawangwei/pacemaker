/*
 * Copyright 2004-2020 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <libgen.h>
#include <signal.h>
#include <bzlib.h>

#include <qb/qbdefs.h>

#include <crm/crm.h>
#include <crm/common/mainloop.h>

unsigned int crm_log_priority = LOG_NOTICE;
unsigned int crm_log_level = LOG_INFO;
static gboolean crm_tracing_enabled(void);
unsigned int crm_trace_nonlog = 0;
bool crm_is_daemon = 0;

GLogFunc glib_log_default;

static void
crm_glib_handler(const gchar * log_domain, GLogLevelFlags flags, const gchar * message,
                 gpointer user_data)
{
    int log_level = LOG_WARNING;
    GLogLevelFlags msg_level = (flags & G_LOG_LEVEL_MASK);
    static struct qb_log_callsite *glib_cs = NULL;

    if (glib_cs == NULL) {
        glib_cs = qb_log_callsite_get(__FUNCTION__, __FILE__, "glib-handler", LOG_DEBUG, __LINE__, crm_trace_nonlog);
    }


    switch (msg_level) {
        case G_LOG_LEVEL_CRITICAL:
            log_level = LOG_CRIT;

            if (crm_is_callsite_active(glib_cs, LOG_DEBUG, 0) == FALSE) {
                /* log and record how we got here */
                crm_abort(__FILE__, __FUNCTION__, __LINE__, message, TRUE, TRUE);
            }
            break;

        case G_LOG_LEVEL_ERROR:
            log_level = LOG_ERR;
            break;
        case G_LOG_LEVEL_MESSAGE:
            log_level = LOG_NOTICE;
            break;
        case G_LOG_LEVEL_INFO:
            log_level = LOG_INFO;
            break;
        case G_LOG_LEVEL_DEBUG:
            log_level = LOG_DEBUG;
            break;

        case G_LOG_LEVEL_WARNING:
        case G_LOG_FLAG_RECURSION:
        case G_LOG_FLAG_FATAL:
        case G_LOG_LEVEL_MASK:
            log_level = LOG_WARNING;
            break;
    }

    do_crm_log(log_level, "%s: %s", log_domain, message);
}

#ifndef NAME_MAX
#  define NAME_MAX 256
#endif

/*!
 * \internal
 * \brief Write out a blackbox (enabling blackboxes if needed)
 *
 * \param[in] nsig  Signal number that was received
 *
 * \note This is a true signal handler, and so must be async-safe.
 */
static void
crm_trigger_blackbox(int nsig)
{
    if(nsig == SIGTRAP) {
        /* Turn it on if it wasn't already */
        crm_enable_blackbox(nsig);
    }
    crm_write_blackbox(nsig, NULL);
}

void
crm_log_deinit(void)
{
    g_log_set_default_handler(glib_log_default, NULL);
}

#define FMT_MAX 256

static void
set_format_string(int method, const char *daemon)
{
    if (method == QB_LOG_SYSLOG) {
        // The system log gets a simplified, user-friendly format
        crm_extended_logging(method, QB_FALSE);
        qb_log_format_set(method, "%g %p: %b");

    } else {
        // Everything else gets more detail, for advanced troubleshooting

        int offset = 0;
        char fmt[FMT_MAX];

        if (method > QB_LOG_STDERR) {
            struct utsname res;
            const char *nodename = "localhost";

            if (uname(&res) == 0) {
                nodename = res.nodename;
            }

            // If logging to file, prefix with timestamp, node name, daemon ID
            offset += snprintf(fmt + offset, FMT_MAX - offset,
                               "%%t %s %-20s[%lu] ",
                               nodename, daemon, (unsigned long) getpid());
        }

        // Add function name (in parentheses)
        offset += snprintf(fmt + offset, FMT_MAX - offset, "(%%n");
        if (crm_tracing_enabled()) {
            // When tracing, add file and line number
            offset += snprintf(fmt + offset, FMT_MAX - offset, "@%%f:%%l");
        }
        offset += snprintf(fmt + offset, FMT_MAX - offset, ")");

        // Add tag (if any), severity, and actual message
        offset += snprintf(fmt + offset, FMT_MAX - offset, " %%g\t%%p: %%b");

        CRM_LOG_ASSERT(offset > 0);
        qb_log_format_set(method, fmt);
    }
}

gboolean
crm_add_logfile(const char *filename)
{
    bool is_default = false;
    static int default_fd = -1;
    static gboolean have_logfile = FALSE;
    const char *default_logfile = CRM_LOG_DIR "/pacemaker.log";

    struct stat parent;
    int fd = 0, rc = 0;
    FILE *logfile = NULL;
    char *parent_dir = NULL;
    char *filename_cp;

    if (filename == NULL && have_logfile == FALSE) {
        filename = default_logfile;
    }

    if (filename == NULL) {
        return FALSE;           /* Nothing to do */
    } else if(safe_str_eq(filename, "none")) {
        return FALSE;           /* Nothing to do */
    } else if(safe_str_eq(filename, "/dev/null")) {
        return FALSE;           /* Nothing to do */
    } else if(safe_str_eq(filename, default_logfile)) {
        is_default = TRUE;
    }

    if(is_default && default_fd >= 0) {
        return TRUE;           /* Nothing to do */
    }

    /* Check the parent directory */
    filename_cp = strdup(filename);
    parent_dir = dirname(filename_cp);
    rc = stat(parent_dir, &parent);

    if (rc != 0) {
        crm_err("Directory '%s' does not exist: logging to '%s' is disabled", parent_dir, filename);
        free(filename_cp);
        return FALSE;
    }
    free(filename_cp);

    errno = 0;
    logfile = fopen(filename, "a");
    if(logfile == NULL) {
        crm_err("%s (%d): Logging to '%s' as uid=%u, gid=%u is disabled",
                pcmk_strerror(errno), errno, filename, geteuid(), getegid());
        return FALSE;
    }

    /* Check/Set permissions if we're root */
    if (geteuid() == 0) {
        struct stat st;
        uid_t pcmk_uid = 0;
        gid_t pcmk_gid = 0;
        gboolean fix = FALSE;
        int logfd = fileno(logfile);
        const char *modestr;
        mode_t filemode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;

        rc = fstat(logfd, &st);
        if (rc < 0) {
            crm_perror(LOG_WARNING, "Cannot stat %s", filename);
            fclose(logfile);
            return FALSE;
        }

        if (pcmk_daemon_user(&pcmk_uid, &pcmk_gid) == 0) {
            if (st.st_gid != pcmk_gid) {
                /* Wrong group */
                fix = TRUE;
            } else if ((st.st_mode & S_IRWXG) != (S_IRGRP | S_IWGRP)) {
                /* Not read/writable by the correct group */
                fix = TRUE;
            }
        }

        modestr = getenv("PCMK_logfile_mode");
        if (modestr) {
            long filemode_l = strtol(modestr, NULL, 8);
            if (filemode_l != LONG_MIN && filemode_l != LONG_MAX) {
                filemode = (mode_t)filemode_l;
	    }
        }

        if (fix) {
            rc = fchown(logfd, pcmk_uid, pcmk_gid);
            if (rc < 0) {
                crm_warn("Cannot change the ownership of %s to user %s and gid %d",
                         filename, CRM_DAEMON_USER, pcmk_gid);
            }
	}

	if (filemode) {
            rc = fchmod(logfd, filemode);
            if (rc < 0) {
                crm_warn("Cannot change the mode of %s to %o", filename, filemode);
            }

            fprintf(logfile, "Set r/w permissions for uid=%d, gid=%d on %s\n",
                    pcmk_uid, pcmk_gid, filename);
            if (fflush(logfile) < 0 || fsync(logfd) < 0) {
                crm_err("Couldn't write out logfile: %s", filename);
            }
        }
    }

    /* Close and reopen with libqb */
    fclose(logfile);
    fd = qb_log_file_open(filename);

    if (fd < 0) {
        crm_perror(LOG_WARNING, "Couldn't send additional logging to %s", filename);
        return FALSE;
    }

    if(is_default) {
        default_fd = fd;

        // Some resource agents will log only if environment variable is set
        if (pcmk__env_option("logfile") == NULL) {
            pcmk__set_env_option("logfile", filename);
        }

    } else if(default_fd >= 0) {
        crm_notice("Switching to %s", filename);
        qb_log_ctl(default_fd, QB_LOG_CONF_ENABLED, QB_FALSE);
    }

    crm_notice("Additional logging available in %s", filename);
    qb_log_ctl(fd, QB_LOG_CONF_ENABLED, QB_TRUE);
    /* qb_log_ctl(fd, QB_LOG_CONF_FILE_SYNC, 1);  Turn on synchronous writes */

#ifdef HAVE_qb_log_conf_QB_LOG_CONF_MAX_LINE_LEN
    // Longer than default, for logging long XML lines
    qb_log_ctl(fd, QB_LOG_CONF_MAX_LINE_LEN, 800);
#endif

    /* Enable callsites */
    crm_update_callsites();
    have_logfile = TRUE;

    return TRUE;
}

static int blackbox_trigger = 0;
static volatile char *blackbox_file_prefix = NULL;

#ifdef QB_FEATURE_LOG_HIRES_TIMESTAMPS
typedef struct timespec *log_time_t;
#else
typedef time_t log_time_t;
#endif

static void
blackbox_logger(int32_t t, struct qb_log_callsite *cs, log_time_t timestamp,
                const char *msg)
{
    if(cs && cs->priority < LOG_ERR) {
        crm_write_blackbox(SIGTRAP, cs); /* Bypass the over-dumping logic */
    } else {
        crm_write_blackbox(0, cs);
    }
}

static void
crm_control_blackbox(int nsig, bool enable)
{
    int lpc = 0;

    if (blackbox_file_prefix == NULL) {
        pid_t pid = getpid();

        blackbox_file_prefix = crm_strdup_printf("%s/%s-%lu",
                                                 CRM_BLACKBOX_DIR,
                                                 crm_system_name,
                                                 (unsigned long) pid);
    }

    if (enable && qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_STATE_GET, 0) != QB_LOG_STATE_ENABLED) {
        qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_SIZE, 5 * 1024 * 1024); /* Any size change drops existing entries */
        qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_ENABLED, QB_TRUE);      /* Setting the size seems to disable it */

        /* Enable synchronous logging */
        for (lpc = QB_LOG_BLACKBOX; lpc < QB_LOG_TARGET_MAX; lpc++) {
            qb_log_ctl(lpc, QB_LOG_CONF_FILE_SYNC, QB_TRUE);
        }

        crm_notice("Initiated blackbox recorder: %s", blackbox_file_prefix);

        /* Save to disk on abnormal termination */
        crm_signal_handler(SIGSEGV, crm_trigger_blackbox);
        crm_signal_handler(SIGABRT, crm_trigger_blackbox);
        crm_signal_handler(SIGILL,  crm_trigger_blackbox);
        crm_signal_handler(SIGBUS,  crm_trigger_blackbox);
        crm_signal_handler(SIGFPE,  crm_trigger_blackbox);

        crm_update_callsites();

        blackbox_trigger = qb_log_custom_open(blackbox_logger, NULL, NULL, NULL);
        qb_log_ctl(blackbox_trigger, QB_LOG_CONF_ENABLED, QB_TRUE);
        crm_trace("Trigger: %d is %d %d", blackbox_trigger,
                  qb_log_ctl(blackbox_trigger, QB_LOG_CONF_STATE_GET, 0), QB_LOG_STATE_ENABLED);

        crm_update_callsites();

    } else if (!enable && qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_STATE_GET, 0) == QB_LOG_STATE_ENABLED) {
        qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_ENABLED, QB_FALSE);

        /* Disable synchronous logging again when the blackbox is disabled */
        for (lpc = QB_LOG_BLACKBOX; lpc < QB_LOG_TARGET_MAX; lpc++) {
            qb_log_ctl(lpc, QB_LOG_CONF_FILE_SYNC, QB_FALSE);
        }
    }
}

void
crm_enable_blackbox(int nsig)
{
    crm_control_blackbox(nsig, TRUE);
}

void
crm_disable_blackbox(int nsig)
{
    crm_control_blackbox(nsig, FALSE);
}

/*!
 * \internal
 * \brief Write out a blackbox, if blackboxes are enabled
 *
 * \param[in] nsig  Signal that was received
 * \param[in] cs    libqb callsite
 *
 * \note This may be called via a true signal handler and so must be async-safe.
 * @TODO actually make this async-safe
 */
void
crm_write_blackbox(int nsig, struct qb_log_callsite *cs)
{
    static volatile int counter = 1;
    static volatile time_t last = 0;

    char buffer[NAME_MAX];
    time_t now = time(NULL);

    if (blackbox_file_prefix == NULL) {
        return;
    }

    switch (nsig) {
        case 0:
        case SIGTRAP:
            /* The graceful case - such as assertion failure or user request */

            if (nsig == 0 && now == last) {
                /* Prevent over-dumping */
                return;
            }

            snprintf(buffer, NAME_MAX, "%s.%d", blackbox_file_prefix, counter++);
            if (nsig == SIGTRAP) {
                crm_notice("Blackbox dump requested, please see %s for contents", buffer);

            } else if (cs) {
                syslog(LOG_NOTICE,
                       "Problem detected at %s:%d (%s), please see %s for additional details",
                       cs->function, cs->lineno, cs->filename, buffer);
            } else {
                crm_notice("Problem detected, please see %s for additional details", buffer);
            }

            last = now;
            qb_log_blackbox_write_to_file(buffer);

            /* Flush the existing contents
             * A size change would also work
             */
            qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_ENABLED, QB_FALSE);
            qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_ENABLED, QB_TRUE);
            break;

        default:
            /* Do as little as possible, just try to get what we have out
             * We logged the filename when the blackbox was enabled
             */
            crm_signal_handler(nsig, SIG_DFL);
            qb_log_blackbox_write_to_file((const char *)blackbox_file_prefix);
            qb_log_ctl(QB_LOG_BLACKBOX, QB_LOG_CONF_ENABLED, QB_FALSE);
            raise(nsig);
            break;
    }
}

gboolean
crm_log_cli_init(const char *entity)
{
    return crm_log_init(entity, LOG_ERR, FALSE, FALSE, 0, NULL, TRUE);
}

static const char *
crm_quark_to_string(uint32_t tag)
{
    const char *text = g_quark_to_string(tag);

    if (text) {
        return text;
    }
    return "";
}

static void
crm_log_filter_source(int source, const char *trace_files, const char *trace_fns,
                      const char *trace_fmts, const char *trace_tags, const char *trace_blackbox,
                      struct qb_log_callsite *cs)
{
    if (qb_log_ctl(source, QB_LOG_CONF_STATE_GET, 0) != QB_LOG_STATE_ENABLED) {
        return;
    } else if (cs->tags != crm_trace_nonlog && source == QB_LOG_BLACKBOX) {
        /* Blackbox gets everything if enabled */
        qb_bit_set(cs->targets, source);

    } else if (source == blackbox_trigger && blackbox_trigger > 0) {
        /* Should this log message result in the blackbox being dumped */
        if (cs->priority <= LOG_ERR) {
            qb_bit_set(cs->targets, source);

        } else if (trace_blackbox) {
            char *key = crm_strdup_printf("%s:%d", cs->function, cs->lineno);

            if (strstr(trace_blackbox, key) != NULL) {
                qb_bit_set(cs->targets, source);
            }
            free(key);
        }

    } else if (source == QB_LOG_SYSLOG) {       /* No tracing to syslog */
        if (cs->priority <= crm_log_priority && cs->priority <= crm_log_level) {
            qb_bit_set(cs->targets, source);
        }
        /* Log file tracing options... */
    } else if (cs->priority <= crm_log_level) {
        qb_bit_set(cs->targets, source);
    } else if (trace_files && strstr(trace_files, cs->filename) != NULL) {
        qb_bit_set(cs->targets, source);
    } else if (trace_fns && strstr(trace_fns, cs->function) != NULL) {
        qb_bit_set(cs->targets, source);
    } else if (trace_fmts && strstr(trace_fmts, cs->format) != NULL) {
        qb_bit_set(cs->targets, source);
    } else if (trace_tags
               && cs->tags != 0
               && cs->tags != crm_trace_nonlog && g_quark_to_string(cs->tags) != NULL) {
        qb_bit_set(cs->targets, source);
    }
}

static void
crm_log_filter(struct qb_log_callsite *cs)
{
    int lpc = 0;
    static int need_init = 1;
    static const char *trace_fns = NULL;
    static const char *trace_tags = NULL;
    static const char *trace_fmts = NULL;
    static const char *trace_files = NULL;
    static const char *trace_blackbox = NULL;

    if (need_init) {
        need_init = 0;
        trace_fns = getenv("PCMK_trace_functions");
        trace_fmts = getenv("PCMK_trace_formats");
        trace_tags = getenv("PCMK_trace_tags");
        trace_files = getenv("PCMK_trace_files");
        trace_blackbox = getenv("PCMK_trace_blackbox");

        if (trace_tags != NULL) {
            uint32_t tag;
            char token[500];
            const char *offset = NULL;
            const char *next = trace_tags;

            do {
                offset = next;
                next = strchrnul(offset, ',');
                snprintf(token, sizeof(token), "%.*s", (int)(next - offset), offset);

                tag = g_quark_from_string(token);
                crm_info("Created GQuark %u from token '%s' in '%s'", tag, token, trace_tags);

                if (next[0] != 0) {
                    next++;
                }

            } while (next != NULL && next[0] != 0);
        }
    }

    cs->targets = 0;            /* Reset then find targets to enable */
    for (lpc = QB_LOG_SYSLOG; lpc < QB_LOG_TARGET_MAX; lpc++) {
        crm_log_filter_source(lpc, trace_files, trace_fns, trace_fmts, trace_tags, trace_blackbox,
                              cs);
    }
}

gboolean
crm_is_callsite_active(struct qb_log_callsite *cs, uint8_t level, uint32_t tags)
{
    gboolean refilter = FALSE;

    if (cs == NULL) {
        return FALSE;
    }

    if (cs->priority != level) {
        cs->priority = level;
        refilter = TRUE;
    }

    if (cs->tags != tags) {
        cs->tags = tags;
        refilter = TRUE;
    }

    if (refilter) {
        crm_log_filter(cs);
    }

    if (cs->targets == 0) {
        return FALSE;
    }
    return TRUE;
}

void
crm_update_callsites(void)
{
    static gboolean log = TRUE;

    if (log) {
        log = FALSE;
        crm_debug
            ("Enabling callsites based on priority=%d, files=%s, functions=%s, formats=%s, tags=%s",
             crm_log_level, getenv("PCMK_trace_files"), getenv("PCMK_trace_functions"),
             getenv("PCMK_trace_formats"), getenv("PCMK_trace_tags"));
    }
    qb_log_filter_fn_set(crm_log_filter);
}

static gboolean
crm_tracing_enabled(void)
{
    if (crm_log_level == LOG_TRACE) {
        return TRUE;
    } else if (getenv("PCMK_trace_files") || getenv("PCMK_trace_functions")
               || getenv("PCMK_trace_formats") || getenv("PCMK_trace_tags")) {
        return TRUE;
    }
    return FALSE;
}

static int
crm_priority2int(const char *name)
{
    struct syslog_names {
        const char *name;
        int priority;
    };
    static struct syslog_names p_names[] = {
        {"emerg", LOG_EMERG},
        {"alert", LOG_ALERT},
        {"crit", LOG_CRIT},
        {"error", LOG_ERR},
        {"warning", LOG_WARNING},
        {"notice", LOG_NOTICE},
        {"info", LOG_INFO},
        {"debug", LOG_DEBUG},
        {NULL, -1}
    };
    int lpc;

    for (lpc = 0; name != NULL && p_names[lpc].name != NULL; lpc++) {
        if (crm_str_eq(p_names[lpc].name, name, TRUE)) {
            return p_names[lpc].priority;
        }
    }
    return crm_log_priority;
}


static void
crm_identity(const char *entity, int argc, char **argv) 
{
    if(crm_system_name != NULL) {
        /* Nothing to do */

    } else if (entity) {
        free(crm_system_name);
        crm_system_name = strdup(entity);

    } else if (argc > 0 && argv != NULL) {
        char *mutable = strdup(argv[0]);
        char *modified = basename(mutable);

        if (strstr(modified, "lt-") == modified) {
            modified += 3;
        }

        free(crm_system_name);
        crm_system_name = strdup(modified);
        free(mutable);

    } else if (crm_system_name == NULL) {
        crm_system_name = strdup("Unknown");
    }

    setenv("PCMK_service", crm_system_name, 1);
}


void
crm_log_preinit(const char *entity, int argc, char **argv) 
{
    /* Configure libqb logging with nothing turned on */

    int lpc = 0;
    int32_t qb_facility = 0;

    static bool have_logging = FALSE;

    if(have_logging == FALSE) {
        have_logging = TRUE;

        crm_xml_init(); /* Sets buffer allocation strategy */

        if (crm_trace_nonlog == 0) {
            crm_trace_nonlog = g_quark_from_static_string("Pacemaker non-logging tracepoint");
        }

        umask(S_IWGRP | S_IWOTH | S_IROTH);

        /* Redirect messages from glib functions to our handler */
        glib_log_default = g_log_set_default_handler(crm_glib_handler, NULL);

        /* and for good measure... - this enum is a bit field (!) */
        g_log_set_always_fatal((GLogLevelFlags) 0); /*value out of range */

        /* Who do we log as */
        crm_identity(entity, argc, argv);

        qb_facility = qb_log_facility2int("local0");
        qb_log_init(crm_system_name, qb_facility, LOG_ERR);
        crm_log_level = LOG_CRIT;

        /* Nuke any syslog activity until it's asked for */
        qb_log_ctl(QB_LOG_SYSLOG, QB_LOG_CONF_ENABLED, QB_FALSE);
#ifdef HAVE_qb_log_conf_QB_LOG_CONF_MAX_LINE_LEN
        // Shorter than default, generous for what we *should* send to syslog
        qb_log_ctl(QB_LOG_SYSLOG, QB_LOG_CONF_MAX_LINE_LEN, 256);
#endif

        /* Set format strings and disable threading
         * Pacemaker and threads do not mix well (due to the amount of forking)
         */
        qb_log_tags_stringify_fn_set(crm_quark_to_string);
        for (lpc = QB_LOG_SYSLOG; lpc < QB_LOG_TARGET_MAX; lpc++) {
            qb_log_ctl(lpc, QB_LOG_CONF_THREADED, QB_FALSE);
#ifdef HAVE_qb_log_conf_QB_LOG_CONF_ELLIPSIS
            // End truncated lines with '...'
            qb_log_ctl(lpc, QB_LOG_CONF_ELLIPSIS, QB_TRUE);
#endif
            set_format_string(lpc, crm_system_name);
        }
    }
}

gboolean
crm_log_init(const char *entity, uint8_t level, gboolean daemon, gboolean to_stderr,
             int argc, char **argv, gboolean quiet)
{
    const char *syslog_priority = NULL;
    const char *logfile = pcmk__env_option("logfile");
    const char *facility = pcmk__env_option("logfacility");
    const char *f_copy = facility;

    crm_is_daemon = daemon;
    crm_log_preinit(entity, argc, argv);

    if (level > LOG_TRACE) {
        level = LOG_TRACE;
    }
    if(level > crm_log_level) {
        crm_log_level = level;
    }

    /* Should we log to syslog */
    if (facility == NULL) {
        if(crm_is_daemon) {
            facility = "daemon";
        } else {
            facility = "none";
        }
        pcmk__set_env_option("logfacility", facility);
    }

    if (safe_str_eq(facility, "none")) {
        quiet = TRUE;


    } else {
        qb_log_ctl(QB_LOG_SYSLOG, QB_LOG_CONF_FACILITY, qb_log_facility2int(facility));
    }

    if (pcmk__env_option_enabled(crm_system_name, "debug")) {
        /* Override the default setting */
        crm_log_level = LOG_DEBUG;
    }

    /* What lower threshold do we have for sending to syslog */
    syslog_priority = pcmk__env_option("logpriority");
    if(syslog_priority) {
        int priority = crm_priority2int(syslog_priority);
        crm_log_priority = priority;
	qb_log_filter_ctl(QB_LOG_SYSLOG, QB_LOG_FILTER_ADD, QB_LOG_FILTER_FILE, "*", priority);
    } else {
	qb_log_filter_ctl(QB_LOG_SYSLOG, QB_LOG_FILTER_ADD, QB_LOG_FILTER_FILE, "*", LOG_NOTICE);
    }

    // Log to syslog unless requested to be quiet
    if (!quiet) {
        qb_log_ctl(QB_LOG_SYSLOG, QB_LOG_CONF_ENABLED, QB_TRUE);
    }

    /* Should we log to stderr */ 
    if (pcmk__env_option_enabled(crm_system_name, "stderr")) {
        /* Override the default setting */
        to_stderr = TRUE;
    }
    crm_enable_stderr(to_stderr);

    /* Should we log to a file */
    if (safe_str_eq("none", logfile)) {
        /* No soup^Hlogs for you! */
    } else if(crm_is_daemon) {
        // Daemons always get a log file, unless explicitly set to "none"
        crm_add_logfile(logfile);
    } else if(logfile) {
        crm_add_logfile(logfile);
    }

    if (crm_is_daemon && pcmk__env_option_enabled(crm_system_name, "blackbox")) {
        crm_enable_blackbox(0);
    }

    /* Summary */
    crm_trace("Quiet: %d, facility %s", quiet, f_copy);
    pcmk__env_option("logfile");
    pcmk__env_option("logfacility");

    crm_update_callsites();

    /* Ok, now we can start logging... */
    if (quiet == FALSE && crm_is_daemon == FALSE) {
        crm_log_args(argc, argv);
    }

    if (crm_is_daemon) {
        const char *user = getenv("USER");

        if (user != NULL && pcmk__str_none_of(user, "root", CRM_DAEMON_USER, NULL)) {
            crm_trace("Not switching to corefile directory for %s", user);
            crm_is_daemon = FALSE;
        }
    }

    if (crm_is_daemon) {
        int user = getuid();
        const char *base = CRM_CORE_DIR;
        struct passwd *pwent = getpwuid(user);

        if (pwent == NULL) {
            crm_perror(LOG_ERR, "Cannot get name for uid: %d", user);

        } else if (pcmk__str_none_of(pwent->pw_name, "root", CRM_DAEMON_USER, NULL)) {
            crm_trace("Don't change active directory for regular user: %s", pwent->pw_name);

        } else if (chdir(base) < 0) {
            crm_perror(LOG_INFO, "Cannot change active directory to %s", base);

        } else {
            crm_info("Changed active directory to %s", base);
#if 0
            {
                char path[512];

                snprintf(path, 512, "%s-%lu", crm_system_name, (unsigned long) getpid());
                mkdir(path, 0750);
                chdir(path);
                crm_info("Changed active directory to %s/%s/%s", base, pwent->pw_name, path);
            }
#endif
        }

        /* Original meanings from signal(7)
         *
         * Signal       Value     Action   Comment
         * SIGTRAP        5        Core    Trace/breakpoint trap
         * SIGUSR1     30,10,16    Term    User-defined signal 1
         * SIGUSR2     31,12,17    Term    User-defined signal 2
         *
         * Our usage is as similar as possible
         */
        mainloop_add_signal(SIGUSR1, crm_enable_blackbox);
        mainloop_add_signal(SIGUSR2, crm_disable_blackbox);
        mainloop_add_signal(SIGTRAP, crm_trigger_blackbox);
    }

    return TRUE;
}

/* returns the old value */
unsigned int
set_crm_log_level(unsigned int level)
{
    unsigned int old = crm_log_level;

    if (level > LOG_TRACE) {
        level = LOG_TRACE;
    }
    crm_log_level = level;
    crm_update_callsites();
    crm_trace("New log level: %d", level);
    return old;
}

void
crm_enable_stderr(int enable)
{
    if (enable && qb_log_ctl(QB_LOG_STDERR, QB_LOG_CONF_STATE_GET, 0) != QB_LOG_STATE_ENABLED) {
        qb_log_ctl(QB_LOG_STDERR, QB_LOG_CONF_ENABLED, QB_TRUE);
        crm_update_callsites();

    } else if (enable == FALSE) {
        qb_log_ctl(QB_LOG_STDERR, QB_LOG_CONF_ENABLED, QB_FALSE);
    }
}

void
crm_bump_log_level(int argc, char **argv)
{
    static int args = TRUE;
    int level = crm_log_level;

    if (args && argc > 1) {
        crm_log_args(argc, argv);
    }

    if (qb_log_ctl(QB_LOG_STDERR, QB_LOG_CONF_STATE_GET, 0) == QB_LOG_STATE_ENABLED) {
        set_crm_log_level(level + 1);
    }

    /* Enable after potentially logging the argstring, not before */
    crm_enable_stderr(TRUE);
}

unsigned int
get_crm_log_level(void)
{
    return crm_log_level;
}

#define ARGS_FMT "Invoked: %s"
void
crm_log_args(int argc, char **argv)
{
    int lpc = 0;
    int len = 0;
    int existing_len = 0;
    int line = __LINE__;
    static int logged = 0;

    char *arg_string = NULL;

    if (argc == 0 || argv == NULL || logged) {
        return;
    }

    logged = 1;

    // cppcheck seems not to understand the abort logic in realloc_safe
    // cppcheck-suppress memleak
    for (; lpc < argc; lpc++) {
        if (argv[lpc] == NULL) {
            break;
        }

        len = 2 + strlen(argv[lpc]);    /* +1 space, +1 EOS */
        arg_string = realloc_safe(arg_string, len + existing_len);
        existing_len += sprintf(arg_string + existing_len, "%s ", argv[lpc]);
    }

    qb_log_from_external_source(__func__, __FILE__, ARGS_FMT, LOG_NOTICE, line, 0, arg_string);

    free(arg_string);
}

void
crm_log_output_fn(const char *file, const char *function, int line, int level, const char *prefix,
                  const char *output)
{
    const char *next = NULL;
    const char *offset = NULL;

    if (level == LOG_NEVER) {
        return;
    }

    if (output == NULL) {
        if (level != LOG_STDOUT) {
            level = LOG_TRACE;
        }
        output = "-- empty --";
    }

    next = output;
    do {
        offset = next;
        next = strchrnul(offset, '\n');
        do_crm_log_alias(level, file, function, line, "%s [ %.*s ]", prefix,
                         (int)(next - offset), offset);
        if (next[0] != 0) {
            next++;
        }

    } while (next != NULL && next[0] != 0);
}
