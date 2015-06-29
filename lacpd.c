/*
 * Copyright (C) 2015 Hewlett-Packard Development Company, L.P.
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013, 2014 Nicira, Inc.
 * All Rights Reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License"); you may
 *   not use this file except in compliance with the License. You may obtain
 *   a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *   WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 *   License for the specific language governing permissions and limitations
 *   under the License.
 */

/************************************************************************//**
 * @ingroup lacpd
 *
 * @file
 * Main source file for the LACP daemon.
 *
 *    The lacpd daemon operates as an overall Halon Link Aggregation (LAG)
 *    Daemon supporting both static LAGs and LACP based dynamic LAGs.
 *
 *    Its purpose in life is:
 *
 *       1. During start up, read port and interface related
 *          configuration data and maintain local cache.
 *       2. During operations, receive administrative
 *          configuration changes and apply to the hardware.
 *       3. Manage static LAG configuration and apply to the hardware.
 *       4. Manage LACP protocol operation for LACP LAGs.
 *       5. Dynamically configure hardware based on
 *          operational state changes as needed.
 ***************************************************************************/
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <config.h>
#include <command-line.h>
#include <compiler.h>
#include <daemon.h>
#include <dirs.h>
#include <dynamic-string.h>
#include <fatal-signal.h>
#include <ovsdb-idl.h>
#include <poll-loop.h>
#include <unixctl.h>
#include <util.h>
#include <openvswitch/vconn.h>
#include <openvswitch/vlog.h>
#include <vswitch-idl.h>
#include <openhalon-idl.h>
#include <hash.h>
#include <shash.h>

#include <nemo/mqueue.h>
#include <nemo/protocol/drivers/mlacp.h>
#include <nemo/pm/pm_cmn.h>
#include <nemo/lacp/lacp_cmn.h>
#include <nemo/lacp/mlacp_debug.h>
#include "lacp.h"
#include "lacp_support.h"

#include "mlacp_fproto.h"
#include "lacp_halon_if.h"

VLOG_DEFINE_THIS_MODULE(lacpd);

static unixctl_cb_func lacpd_unixctl_dump;
static unixctl_cb_func halon_lacpd_exit;
static bool exiting = false;

/* Forward declaration */
static void lacpd_exit(void);

/**
 * ovs-appctl interface callback function to dump internal debug information.
 * This top level debug dump function calls other functions to dump lacpd
 * daemon's internal data. The function arguments in argv are used to
 * control the debug output.
 *
 * @param conn connection to ovs-appctl interface.
 * @param argc number of arguments.
 * @param argv array of arguments.
 * @param OVS_UNUSED aux argument not used.
 */
static void
lacpd_unixctl_dump(struct unixctl_conn *conn, int argc,
                   const char *argv[], void *aux OVS_UNUSED)
{
    struct ds ds = DS_EMPTY_INITIALIZER;

    lacpd_debug_dump(&ds, argc, argv);

    unixctl_command_reply(conn, ds_cstr(&ds));
    ds_destroy(&ds);
} /* lacpd_unixctl_dump */

/**
 * lacpd daemon's timer handler function.
 */
static void
timerHandler(void)
{
    ML_event *timerEvent;

    timerEvent = (ML_event *)malloc(sizeof(ML_event));
    if (NULL == timerEvent) {
        RDEBUG(DL_ERROR, "Out of memory for LACP timer message.\n");
        return;
    }
    memset(timerEvent, 0, sizeof(ML_event));
    timerEvent->sender.peer = ml_timer_index;

    ml_send_event(timerEvent);
} /* timerHandler */

/**
 * lacpd daemon's main OVS interface function.
 *
 * @param arg pointer to ovs-appctl server struct.
 */
void *
lacpd_ovs_main_thread(void *arg)
{
    struct unixctl_server *appctl;

    appctl = (struct unixctl_server *)arg;

    exiting = false;
    while (!exiting) {
        lacpd_run();
        unixctl_server_run(appctl);

        lacpd_wait();
        unixctl_server_wait(appctl);
        if (exiting) {
            poll_immediate_wake();
        } else {
            poll_block();
        }
    }

    lacpd_exit();
    unixctl_server_destroy(appctl);

    /* HALON_TODO -- need to tell main loop to exit... */

} /* lacpd_ovs_main_thread */

/**
 * lacpd daemon's main initialization function.
 *
 * @param db_path pathname for OVSDB connection.
 */
static void
lacpd_init(const char *db_path, struct unixctl_server *appctl)
{
    int rc;
    sigset_t sigset;
    pthread_t ovs_if_thread;
    pthread_t lacpd_thread;

    /* Initialize LACP main task event receiver sockets. */
    hc_enet_init_event_rcvr();

    /**************** Thread Related ***************/

    /* Block all signals so the spawned threads don't receive any. */
    sigemptyset(&sigset);
    sigfillset(&sigset);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

#if 0
    /* ...HALON_TODO... */
    /* Spawn off the main Cyclone LACP protocol thread. */
    rc = pthread_create(&lacpd_thread,
                        (pthread_attr_t *)NULL,
                        lacpd_protocol_thread,
                        NULL);
    if (rc) {
        VLOG_ERR("pthread_create for LACPD protocol thread failed! rc=%d", rc);
        exit(-rc);
    }
#endif

    /* Initialize IDL through a new connection to the dB. */
    lacpd_ovsdb_if_init(db_path);

    /* Register ovs-appctl commands for this daemon. */
    unixctl_command_register("lacpd/dump", "", 0, 2, lacpd_unixctl_dump, NULL);

    /* Spawn off the OVSDB interface thread. */
    rc = pthread_create(&ovs_if_thread,
                        (pthread_attr_t *)NULL,
                        lacpd_ovs_main_thread,
                        (void *)appctl);
    if (rc) {
        VLOG_ERR("pthread_create for OVSDB i/f thread failed! rc=%d", rc);
        exit(-rc);
    }

} /* lacpd_init */

/**
 * Cleanup function at daemon shutdown time.
 *
 */
static void
lacpd_exit(void)
{
    lacpd_ovsdb_if_exit();
    VLOG_INFO("lacpd OVSDB thread exiting...");
} /* lacpd_exit */

/**
 * lacpd usage help function.
 *
 */
static void
usage(void)
{
    printf("%s: Halon LACP daemon\n"
           "usage: %s [OPTIONS] [DATABASE]\n"
           "where DATABASE is a socket on which ovsdb-server is listening\n"
           "      (default: \"unix:%s/db.sock\").\n",
           program_name, program_name, ovs_rundir());
    daemon_usage();
    vlog_usage();
    printf("\nOther options:\n"
           "  --unixctl=SOCKET        override default control socket name\n"
           "  -h, --help              display this help message\n");
    exit(EXIT_SUCCESS);
} /* usage */

static char *
parse_options(int argc, char *argv[], char **unixctl_pathp)
{
    enum {
        OPT_UNIXCTL = UCHAR_MAX + 1,
        VLOG_OPTION_ENUMS,
        DAEMON_OPTION_ENUMS,
    };
    static const struct option long_options[] = {
        {"help",        no_argument, NULL, 'h'},
        {"unixctl",     required_argument, NULL, OPT_UNIXCTL},
        DAEMON_LONG_OPTIONS,
        VLOG_LONG_OPTIONS,
        {NULL, 0, NULL, 0},
    };
    char *short_options = long_options_to_short_options(long_options);

    for (;;) {
        int c;

        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            usage();

        case OPT_UNIXCTL:
            *unixctl_pathp = optarg;
            break;

        VLOG_OPTION_HANDLERS
        DAEMON_OPTION_HANDLERS

        case '?':
            exit(EXIT_FAILURE);

        default:
            abort();
        }
    }
    free(short_options);

    argc -= optind;
    argv += optind;

    switch (argc) {
    case 0:
        return xasprintf("unix:%s/db.sock", ovs_rundir());

    case 1:
        return xstrdup(argv[0]);

    default:
        VLOG_FATAL("at most one non-option argument accepted; "
                   "use --help for usage");
    }
} /* parse_options */

/**
 * lacpd daemon's ovs-appctl callback function for exit command.
 *
 * @param conn is pointer appctl connection data struct.
 * @param argc OVS_UNUSED
 * @param argv OVS_UNUSED
 * @param exiting_ is pointer to a flag that reports exit status.
 */
static void
halon_lacpd_exit(struct unixctl_conn *conn, int argc OVS_UNUSED,
                 const char *argv[] OVS_UNUSED, void *exiting_)
{
    bool *exiting = exiting_;
    *exiting = true;
    unixctl_command_reply(conn, NULL);
} /* halon_lacpd_exit */

/**
 * Main function for lacpd daemon.
 *
 * @param argc is the number of command line arguments.
 * @param argv is an array of command line arguments.
 *
 * @return 0 for success or exit status on daemon exit.
 */
int
main(int argc, char *argv[])
{
    int mlacp_tindex;
    struct itimerval timerVal;
    char *appctl_path = NULL;
    struct unixctl_server *appctl;
    char *ovsdb_sock;
    int retval;
    sigset_t sigset;
    int signum;

    set_program_name(argv[0]);
    proctitle_init(argc, argv);
    fatal_ignore_sigpipe();

    /* Parse command line args and get the name of the OVSDB socket. */
    ovsdb_sock = parse_options(argc, argv, &appctl_path);

    /* Initialize the metadata for the IDL cache. */
    ovsrec_init();

    /* Fork and return in child process; but don't notify parent of
     * startup completion yet. */
    daemonize_start();

    /* Create UDS connection for ovs-appctl. */
    retval = unixctl_server_create(appctl_path, &appctl);
    if (retval) {
        exit(EXIT_FAILURE);
    }

    /* Register the ovs-appctl "exit" command for this daemon. */
    unixctl_command_register("exit", "", 0, 0, halon_lacpd_exit, &exiting);

    /* Initialize Cyclone LACP data structures. */
    (void)mlacp_init(TRUE);

    /* Initialize various protocol and event sockets, and create
       the IDL cache of the dB at ovsdb_sock. */
    lacpd_init(ovsdb_sock, appctl);
    free(ovsdb_sock);

    /* Notify parent of startup completion. */
    daemonize_complete();

    /* Enable asynch log writes to disk. */
    vlog_enable_async();

    VLOG_INFO_ONCE("%s (Halon Link Aggregation Daemon) started", program_name);

    /* Set up timer to fire off every second. */
    timerVal.it_interval.tv_sec  = 1;
    timerVal.it_interval.tv_usec = 0;
    timerVal.it_value.tv_sec  = 1;
    timerVal.it_value.tv_usec = 0;

    if ((mlacp_tindex = setitimer(ITIMER_REAL, &timerVal, NULL)) != 0) {
        VLOG_ERR("lacpd main: Timer start failed!\n");
    }

    /* Wait for all signals in an infinite loop. */
    sigfillset(&sigset);
    while (!lacpd_shutdown) {

        sigwait(&sigset, &signum);
        switch (signum) {

        case SIGALRM:
            timerHandler();
            break;

        case SIGTERM:
        case SIGINT:
            VLOG_WARN("%s, sig %d caught", __FUNCTION__, signum);
            lacpd_shutdown = 1;
            break;

        default:
            VLOG_INFO("Ignoring signal %d.\n", signum);
            break;
        }
    }

    /* HALON_TODO - clean up various threads. */
    /* lacp_halon_cleanup(); */

    return 0;
} /* main */
