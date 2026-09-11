/*
 * moonflare -- Moon Flare TUI client (VDK / libviper).
 */

#include "ui_screen.h"
#include "layout.h"
#include "debug/mf_backtrace.h"

#include <stdio.h>
#include <string.h>

#define DEFAULT_CONNECT "172.16.0.65:5250"

static void usage(const char *prog)
{
    fprintf(stderr,
        "moonflare -- Moon Flare TUI (libviper/VDK)\n"
        "\n"
        "Usage: %s [--connect HOST:PORT] [--config PATH] [--dump-layout [KIND]] [--help]\n"
        "\n"
        "  --connect       moonflared REST address (default %s)\n"
        "  --config        path to moonflare.json\n"
        "  --dump-layout   80x25 ASCII (dashboard|pack|charger|settings|confirm)\n"
        "\n"
        "All views render at 80x25 minimum. F10 opens the menubar.\n",
        prog, DEFAULT_CONNECT);
}

int main(int argc, char **argv)
{
    const char *connect = DEFAULT_CONNECT;
    const char *config  = NULL;
    const char *dump_kind = NULL;
    int help = 0, dump = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--connect") && i + 1 < argc) {
            connect = argv[++i];
        } else if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            config = argv[++i];
        } else if (!strcmp(argv[i], "--dump-layout")) {
            dump = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                dump_kind = argv[++i];
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
    if (dump)
        return mf_tui_dump_layout_main(dump_kind);
    mf_backtrace_install();
    return mf_tui_run(connect, config);
}
