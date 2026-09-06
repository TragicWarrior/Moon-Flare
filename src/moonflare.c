/*
 * moonflare -- Moon Flare TUI client (VDK / libviper).
 *
 * PR-1 stub: --help only. The vk_screen + vk_menubar dashboard is PR-7.
 * Do not link libviper yet.
 */

#include <stdio.h>
#include <string.h>

#define DEFAULT_CONNECT "172.16.0.65:5250"

static void usage(const char *prog)
{
    fprintf(stderr,
        "moonflare -- Moon Flare TUI (libviper/VDK)\n"
        "\n"
        "Usage: %s [--connect HOST:PORT] [--config PATH] [--help]\n"
        "\n"
        "  --connect  moonflared REST address (default %s)\n"
        "  --config   path to moonflare.json (search path is PR-3)\n"
        "\n"
        "All views render at 80x25 minimum. TUI implementation is PR-7.\n",
        prog, DEFAULT_CONNECT);
}

int main(int argc, char **argv)
{
    const char *connect = DEFAULT_CONNECT;
    const char *config  = NULL;
    int help = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--connect") && i + 1 < argc) {
            connect = argv[++i];
        } else if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            config = argv[++i];
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            help = 1;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (help) {
        usage(argv[0]);
        return 0;
    }

    (void)connect;
    (void)config;
    fprintf(stderr, "moonflare: TUI not implemented yet (PR-7)\n");
    usage(argv[0]);
    return 2;
}
