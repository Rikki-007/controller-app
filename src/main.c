/*
 * main.c - the virtual controller daemon (vcd).
 *
 * Listens for input packets, runs each through its session's parser and
 * pushes the result to a virtual gamepad:
 *
 *   network.c  ->  parser.c  ->  hid_output.c
 *
 * There is no handshake yet, so sessions are implicit: the first packet
 * carrying an unknown token opens a slot (up to VC_MAX_SLOTS) using the
 * controller type given on the command line. A slot is neutralised when its
 * client goes quiet and closed when it stays quiet.
 *
 * Single-threaded: everything runs on the main loop.
 */
#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600   /* GetTickCount64 */
#  endif
#  include <windows.h>
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
#include "parser.h"

#define VC_MAX_SLOTS          4
/* How long the loop may sleep before checking timeouts and the stop flag. */
#define POLL_INTERVAL_MS      50
/* Silence after which a pad is neutralised, so no button stays held. */
#define DEFAULT_TIMEOUT_MS    250
/* Silence after which the pad is unplugged and its slot freed. */
#define SLOT_IDLE_MS          10000
/* Pause before trying to open a slot again after a failure. */
#define OPEN_RETRY_MS         2000

typedef struct {
    int              in_use;
    uint32_t         token;
    vc_net_peer_t    peer;
    vc_parser_t      parser;
    vc_hid_device_t *device;
    uint64_t         last_input_ms;
    int              neutral;        /* already neutralised after a timeout */
    vc_input_state_t last_state;     /* for --print */
} slot_t;

typedef struct {
    vc_controller_type_t type;
    const char          *bind_address;
    uint16_t             port;
    unsigned             timeout_ms;
    int                  print;
} options_t;

typedef struct {
    options_t opts;
    slot_t    slots[VC_MAX_SLOTS];
    uint64_t  next_open_ms;      /* no new slots before this time */
    uint64_t  dropped_no_slot;
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
#endif
}

/* ------------------------------------------------------------------------ */
/* Slots                                                                    */
/* ------------------------------------------------------------------------ */

static int slot_index(const app_t *app, const slot_t *slot)
{
    return (int)(slot - app->slots);
}

static void close_slot(app_t *app, slot_t *slot, const char *reason)
{
    const vc_parser_stats_t *stats = &slot->parser.stats;

    printf("slot %d closed (%s): %lu inputs accepted, %lu stale, %lu wrong token\n",
           slot_index(app, slot), reason, (unsigned long)stats->accepted,
           (unsigned long)stats->rejected_stale, (unsigned long)stats->rejected_token);
    vc_hid_destroy(slot->device);
    memset(slot, 0, sizeof *slot);
}

static slot_t *find_slot(app_t *app, uint32_t token)
{
    int i;

    for (i = 0; i < VC_MAX_SLOTS; i++) {
        if (app->slots[i].in_use && app->slots[i].token == token) {
            return &app->slots[i];
        }
    }
    return NULL;
}

static slot_t *open_slot(app_t *app, const vc_net_input_t *input, uint64_t now)
{
    slot_t *slot = NULL;
    int     i;

    if (now < app->next_open_ms) {
        return NULL;
    }
    for (i = 0; i < VC_MAX_SLOTS; i++) {
        if (!app->slots[i].in_use) {
            slot = &app->slots[i];
            break;
        }
    }
    if (slot == NULL) {
        /* Log the 1st, 2nd, 4th... drop, not every packet. */
        app->dropped_no_slot++;
        if ((app->dropped_no_slot & (app->dropped_no_slot - 1)) == 0) {
            fprintf(stderr, "all %d slots are in use; ignoring a new client (%lu packets so far)\n",
                    VC_MAX_SLOTS, (unsigned long)app->dropped_no_slot);
        }
        return NULL;
    }

    memset(slot, 0, sizeof *slot);
    if (vc_parser_init(&slot->parser, app->opts.type, input->packet.token) != 0 ||
        vc_hid_create(app->opts.type, &slot->device) != 0) {
        fprintf(stderr, "could not open a slot; retrying in %d ms\n", OPEN_RETRY_MS);
        app->next_open_ms = now + OPEN_RETRY_MS;
        return NULL;
    }
    slot->in_use = 1;
    slot->token = input->packet.token;
    slot->peer = input->peer;
    slot->last_input_ms = now;

    printf("slot %d opened for %u.%u.%u.%u:%u (token %08lx)\n", slot_index(app, slot),
           input->peer.ip[0], input->peer.ip[1], input->peer.ip[2], input->peer.ip[3],
           (unsigned)input->peer.port, (unsigned long)slot->token);
    return slot;
}

static void print_state(const app_t *app, slot_t *slot, const vc_input_state_t *state)
{
    const vc_input_state_t *last = &slot->last_state;

    if (state->buttons == last->buttons &&
        state->left.x == last->left.x && state->left.y == last->left.y &&
        state->right.x == last->right.x && state->right.y == last->right.y &&
        state->trigger_left == last->trigger_left &&
        state->trigger_right == last->trigger_right) {
        return;
    }
    printf("slot %d: buttons=%05lx left=(%6d,%6d) right=(%6d,%6d) triggers=(%3u,%3u)\n",
           slot_index(app, slot), (unsigned long)state->buttons,
           state->left.x, state->left.y, state->right.x, state->right.y,
           (unsigned)state->trigger_left, (unsigned)state->trigger_right);
    slot->last_state = *state;
}

/* Called by the network listener for every well-formed input packet. */
static void on_input(const vc_net_input_t *input, void *user)
{
    app_t           *app = (app_t *)user;
    uint64_t         now = now_ms();
    vc_input_state_t state;
    slot_t          *slot;

    slot = find_slot(app, input->packet.token);
    if (slot == NULL) {
        slot = open_slot(app, input, now);
        if (slot == NULL) {
            return;
        }
    }

    /* Rejections (stale, duplicate) are counted by the parser. */
    if (vc_parser_accept(&slot->parser, &input->packet, &state) != VC_PARSE_OK) {
        return;
    }
    if (vc_hid_submit(slot->device, &state) != 0) {
        /* The driver already logged why. Drop the pad rather than fail on
         * every packet; the client gets a fresh one after the retry delay. */
        close_slot(app, slot, "driver error");
        app->next_open_ms = now + OPEN_RETRY_MS;
        return;
    }
    slot->last_input_ms = now;
    slot->neutral = 0;
    if (app->opts.print) {
        print_state(app, slot, &state);
    }
}

/* Releases everything on a pad: nothing pressed, sticks centred. */
static void neutralise(slot_t *slot)
{
    vc_input_state_t neutral;

    memset(&neutral, 0, sizeof neutral);
    vc_hid_submit(slot->device, &neutral);
    slot->last_state = neutral;
    slot->neutral = 1;
}

static void check_timeouts(app_t *app)
{
    uint64_t now = now_ms();
    int      i;

    for (i = 0; i < VC_MAX_SLOTS; i++) {
        slot_t  *slot = &app->slots[i];
        uint64_t quiet;

        if (!slot->in_use) {
            continue;
        }
        quiet = now - slot->last_input_ms;
        if (quiet >= SLOT_IDLE_MS) {
            close_slot(app, slot, "client went away");
        } else if (!slot->neutral && quiet >= app->opts.timeout_ms) {
            printf("slot %d: no input for %u ms, releasing all controls\n", i,
                   app->opts.timeout_ms);
            neutralise(slot);
            /* The client may restart its sequence numbers when it resumes. */
            vc_parser_reset(&slot->parser);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Command line                                                             */
/* ------------------------------------------------------------------------ */

static void usage(const char *program)
{
    printf("usage: %s [options]\n"
           "\n"
           "  --type TYPE     controller to emulate: xbox, ps4, ps5, switch (default xbox)\n"
           "  --port PORT     UDP port to listen on (default %d)\n"
           "  --bind ADDR     IPv4 address to listen on (default: all interfaces)\n"
           "  --timeout MS    release all controls after this much silence (default %d)\n"
           "  --print         print each change of state\n"
           "  --help          show this text\n",
           program, VC_DEFAULT_PORT, DEFAULT_TIMEOUT_MS);
}

static int parse_type(const char *text, vc_controller_type_t *out)
{
    static const struct {
        const char          *name;
        vc_controller_type_t type;
    } types[] = {
        { "xbox", VC_CONTROLLER_XBOX },
        { "ps4", VC_CONTROLLER_PS4 },
        { "ps5", VC_CONTROLLER_PS5 },
        { "switch", VC_CONTROLLER_SWITCH }
    };
    size_t i;

    for (i = 0; i < sizeof types / sizeof types[0]; i++) {
        if (strcmp(text, types[i].name) == 0) {
            *out = types[i].type;
            return 0;
        }
    }
    return -1;
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

    opts->type = VC_CONTROLLER_XBOX;
    opts->bind_address = NULL;
    opts->port = VC_DEFAULT_PORT;
    opts->timeout_ms = DEFAULT_TIMEOUT_MS;
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
        }

        if (value == NULL) {
            fprintf(stderr, "%s: missing value or unknown option '%s'\n", argv[0], arg);
            return -1;
        }
        if (strcmp(arg, "--type") == 0) {
            if (parse_type(value, &opts->type) != 0) {
                fprintf(stderr, "%s: unknown controller type '%s'\n", argv[0], value);
                return -1;
            }
        } else if (strcmp(arg, "--port") == 0) {
            if (parse_number(value, 1, 65535, &number) != 0) {
                fprintf(stderr, "%s: '%s' is not a port number\n", argv[0], value);
                return -1;
            }
            opts->port = (uint16_t)number;
        } else if (strcmp(arg, "--bind") == 0) {
            opts->bind_address = value;
        } else if (strcmp(arg, "--timeout") == 0) {
            if (parse_number(value, 1, SLOT_IDLE_MS - 1, &number) != 0) {
                fprintf(stderr, "%s: timeout must be 1..%d ms\n", argv[0], SLOT_IDLE_MS - 1);
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
    static app_t    app;   /* static: zero-initialised and off the stack */
    vc_net_config_t config;
    vc_net_t       *net = NULL;
    int             status = EXIT_SUCCESS;
    int             parsed;
    int             i;

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

    if (vc_hid_init() != 0) {
        return EXIT_FAILURE;
    }

    memset(&config, 0, sizeof config);
    config.bind_address = app.opts.bind_address;
    config.port = app.opts.port;
    config.on_input = on_input;
    config.user = &app;
    if (vc_net_open(&config, &net) != 0) {
        vc_hid_shutdown();
        return EXIT_FAILURE;
    }

    printf("vcd running: %s backend, emulating %s, press Ctrl+C to stop\n",
           vc_hid_backend_name(),
           vc_hid_target_for(app.opts.type) == VC_HID_TARGET_X360 ? "an Xbox 360 pad"
           : vc_hid_target_for(app.opts.type) == VC_HID_TARGET_DS4 ? "a DualShock 4"
           : "an evdev gamepad");
    if (strcmp(vc_hid_backend_name(), "stub") == 0) {
        printf("note: built with the stub backend, so no gamepad appears in the OS "
               "(see docs/BUILD.md)\n");
    }

    while (!g_stop) {
        if (vc_net_poll(net, POLL_INTERVAL_MS) < 0) {
            fprintf(stderr, "the network listener failed; shutting down\n");
            status = EXIT_FAILURE;
            break;
        }
        check_timeouts(&app);
    }

    /* Shutdown order: release and unplug the pads, then the driver, then the
     * socket. Reached on Ctrl+C and on a listener failure alike. */
    if (g_stop) {
        printf("stop requested, shutting down\n");
    }
    for (i = 0; i < VC_MAX_SLOTS; i++) {
        if (app.slots[i].in_use) {
            neutralise(&app.slots[i]);
            close_slot(&app, &app.slots[i], "shutdown");
        }
    }
    vc_hid_shutdown();
    vc_net_close(net);
    printf("vcd stopped\n");
    return status;
}
