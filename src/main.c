/*
 * main.c - the virtual controller daemon (vcd).
 *
 * Wires the network layer to the session layer and runs the loop:
 *
 *   network.c  --TCP bytes-->  session.c (protocol.c)  --> hid_output.c
 *              --UDP input-->  session.c (parser.c)    --> hid_output.c
 *
 * Clients connect over TCP and send HELLO to get a slot, a session token and
 * a virtual gamepad of the type they asked for; then they stream input over
 * UDP with that token. session.h describes the lifecycle and the timeouts.
 *
 * Single-threaded: everything runs on the main loop.
 */
#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600   /* GetTickCount64 */
#  endif
#  include <windows.h>
#  include <bcrypt.h>             /* BCryptGenRandom */
#else
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200112L
#  endif
#  include <time.h>
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controller.h"
#include "hid_output.h"
#include "network.h"
#include "session.h"

/* How long the loop may sleep before checking timeouts and the stop flag. */
#define POLL_INTERVAL_MS 50

typedef struct {
    const char *bind_address;
    uint16_t    port;
    unsigned    timeout_ms;
    int         print;
} options_t;

typedef struct {
    options_t        opts;
    vc_net_t        *net;
    vc_sessions_t    sessions;
    vc_input_state_t printed[VC_MAX_SLOTS];   /* for --print */
} app_t;

/* ------------------------------------------------------------------------ */
/* Platform helpers                                                         */
/* ------------------------------------------------------------------------ */

static uint64_t now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

/* Session tokens come from the OS random generator, so a LAN peer cannot
 * predict the next one. */
static uint32_t random32(void *user)
{
    static int warned;
    static uint32_t counter;
    uint32_t value = 0;
    int      ok;

    (void)user;
#ifdef _WIN32
    ok = BCryptGenRandom(NULL, (PUCHAR)&value, (ULONG)sizeof value,
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
    {
        FILE *f = fopen("/dev/urandom", "rb");
        ok = f != NULL && fread(&value, sizeof value, 1, f) == 1;
        if (f != NULL) {
            fclose(f);
        }
    }
#endif
    if (!ok) {
        /* Still unique and non-zero, just guessable. */
        if (!warned) {
            fprintf(stderr, "warning: no OS random source; session tokens are predictable\n");
            warned = 1;
        }
        value = (uint32_t)now_ms() * 2654435761u + ++counter;
    }
    return value;
}

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void install_signal_handlers(void)
{
#ifdef _WIN32
    /* After the first Ctrl+C Windows restores the default action, so a
     * second one force-quits if shutdown ever hangs. */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGBREAK, on_signal);
#else
    struct sigaction action;

    /* No SA_RESTART: the signal interrupts poll() so the loop notices at once. */
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    /* Writing to a client that has gone away must fail, not kill us. */
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
#endif
}

/* ------------------------------------------------------------------------ */
/* Wiring                                                                   */
/* ------------------------------------------------------------------------ */

/* network.h callbacks -> session.h */

static void on_input(const vc_net_input_t *input, void *user)
{
    vc_sessions_input(&((app_t *)user)->sessions, input, now_ms());
}

static void on_connect(int conn, const vc_net_peer_t *peer, void *user)
{
    vc_sessions_connected(&((app_t *)user)->sessions, conn, peer, now_ms());
}

static void on_data(int conn, const uint8_t *data, size_t size, void *user)
{
    vc_sessions_data(&((app_t *)user)->sessions, conn, data, size, now_ms());
}

static void on_disconnect(int conn, void *user)
{
    vc_sessions_disconnected(&((app_t *)user)->sessions, conn, now_ms());
}

/* session.h callbacks -> network.h and stdout */

static int session_send(int conn, const uint8_t *data, size_t size, void *user)
{
    return vc_net_send(((app_t *)user)->net, conn, data, size);
}

static void session_disconnect(int conn, void *user)
{
    vc_net_disconnect(((app_t *)user)->net, conn);
}

static void session_log(const char *message, void *user)
{
    (void)user;
    printf("%s\n", message);
}

static void session_state(int slot, const vc_input_state_t *state, void *user)
{
    app_t            *app = (app_t *)user;
    vc_input_state_t *last = &app->printed[slot];

    if (!app->opts.print) {
        return;
    }
    if (state->buttons == last->buttons &&
        state->left.x == last->left.x && state->left.y == last->left.y &&
        state->right.x == last->right.x && state->right.y == last->right.y &&
        state->trigger_left == last->trigger_left &&
        state->trigger_right == last->trigger_right) {
        return;
    }
    printf("slot %d: buttons=%05lx left=(%6d,%6d) right=(%6d,%6d) triggers=(%3u,%3u)\n",
           slot, (unsigned long)state->buttons,
           state->left.x, state->left.y, state->right.x, state->right.y,
           (unsigned)state->trigger_left, (unsigned)state->trigger_right);
    *last = *state;
}

/* ------------------------------------------------------------------------ */
/* Command line                                                             */
/* ------------------------------------------------------------------------ */

static void usage(const char *program)
{
    printf("usage: %s [options]\n"
           "\n"
           "  --port PORT     TCP and UDP port to listen on (default %d)\n"
           "  --bind ADDR     IPv4 address to listen on (default: all interfaces)\n"
           "  --timeout MS    release all controls after this much input silence\n"
           "                  (default %d; the session ends after %d ms)\n"
           "  --print         print each change of state\n"
           "  --help          show this text\n"
           "\n"
           "Each client chooses its controller type in its HELLO message.\n",
           program, VC_DEFAULT_PORT, VC_SESSION_INPUT_TIMEOUT_MS,
           VC_SESSION_IDLE_TIMEOUT_MS);
}

static int parse_number(const char *text, long min, long max, long *out)
{
    char *end;
    long  value = strtol(text, &end, 10);

    if (end == text || *end != '\0' || value < min || value > max) {
        return -1;
    }
    *out = value;
    return 0;
}

/* Returns 0 to run, 1 to exit successfully (--help), -1 on a usage error. */
static int parse_options(int argc, char **argv, options_t *opts)
{
    int i;

    opts->bind_address = NULL;
    opts->port = VC_DEFAULT_PORT;
    opts->timeout_ms = VC_SESSION_INPUT_TIMEOUT_MS;
    opts->print = 0;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        long        number;

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 1;
        } else if (strcmp(arg, "--print") == 0) {
            opts->print = 1;
            continue;
        } else if (strcmp(arg, "--type") == 0) {
            fprintf(stderr, "%s: --type was removed: each client now chooses its "
                    "controller type in HELLO (vc_send --type)\n", argv[0]);
            return -1;
        }

        if (value == NULL) {
            fprintf(stderr, "%s: missing value or unknown option '%s'\n", argv[0], arg);
            return -1;
        }
        if (strcmp(arg, "--port") == 0) {
            if (parse_number(value, 1, 65535, &number) != 0) {
                fprintf(stderr, "%s: '%s' is not a port number\n", argv[0], value);
                return -1;
            }
            opts->port = (uint16_t)number;
        } else if (strcmp(arg, "--bind") == 0) {
            opts->bind_address = value;
        } else if (strcmp(arg, "--timeout") == 0) {
            if (parse_number(value, 1, VC_SESSION_IDLE_TIMEOUT_MS - 1, &number) != 0) {
                fprintf(stderr, "%s: timeout must be 1..%d ms\n", argv[0],
                        VC_SESSION_IDLE_TIMEOUT_MS - 1);
                return -1;
            }
            opts->timeout_ms = (unsigned)number;
        } else {
            fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg);
            return -1;
        }
        i++;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                              */
/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    static app_t        app;   /* static: zero-initialised and off the stack */
    vc_net_config_t     net_cfg;
    vc_session_config_t session_cfg;
    int                 status = EXIT_SUCCESS;
    int                 parsed;

    parsed = parse_options(argc, argv, &app.opts);
    if (parsed != 0) {
        if (parsed < 0) {
            usage(argv[0]);
        }
        return parsed > 0 ? EXIT_SUCCESS : 2;
    }

    /* Make status lines appear promptly even when stdout is a pipe or file.
     * Windows has no line buffering (it behaves as full), so go unbuffered. */
#ifdef _WIN32
    setvbuf(stdout, NULL, _IONBF, 0);
#else
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
#endif
    install_signal_handlers();

    memset(&session_cfg, 0, sizeof session_cfg);
    session_cfg.send = session_send;
    session_cfg.disconnect = session_disconnect;
    session_cfg.random32 = random32;
    session_cfg.log = session_log;
    session_cfg.on_state = session_state;
    session_cfg.user = &app;
    session_cfg.input_timeout_ms = app.opts.timeout_ms;
    if (vc_sessions_init(&app.sessions, &session_cfg) != 0) {
        return EXIT_FAILURE;
    }

    if (vc_hid_init() != 0) {
        return EXIT_FAILURE;
    }

    memset(&net_cfg, 0, sizeof net_cfg);
    net_cfg.bind_address = app.opts.bind_address;
    net_cfg.port = app.opts.port;
    net_cfg.on_input = on_input;
    net_cfg.tcp = 1;
    net_cfg.on_connect = on_connect;
    net_cfg.on_data = on_data;
    net_cfg.on_disconnect = on_disconnect;
    net_cfg.user = &app;
    if (vc_net_open(&net_cfg, &app.net) != 0) {
        vc_hid_shutdown();
        return EXIT_FAILURE;
    }

    printf("vcd running: %s backend, up to %d controllers on port %u, press Ctrl+C to stop\n",
           vc_hid_backend_name(), VC_MAX_SLOTS, (unsigned)vc_net_port(app.net));
    if (strcmp(vc_hid_backend_name(), "stub") == 0) {
        printf("note: built with the stub backend, so no gamepad appears in the OS "
               "(see docs/BUILD.md)\n");
    }

    while (!g_stop) {
        if (vc_net_poll(app.net, POLL_INTERVAL_MS) < 0) {
            fprintf(stderr, "the network listener failed; shutting down\n");
            status = EXIT_FAILURE;
            break;
        }
        vc_sessions_tick(&app.sessions, now_ms());
    }

    /* Shutdown order: tell the clients and unplug the pads, then the driver,
     * then the sockets. Reached on Ctrl+C and on a listener failure alike. */
    if (g_stop) {
        printf("stop requested, shutting down\n");
    }
    vc_sessions_shutdown(&app.sessions);
    vc_hid_shutdown();
    vc_net_close(app.net);
    printf("vcd stopped\n");
    return status;
}
