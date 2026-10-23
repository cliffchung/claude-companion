#include <stdio.h>
#include <stdlib.h>
#include "cc_cmd.h"

/* Prints cc_cmd_watch's command line, for firmware/tools/test_watch_loop.py.
   usage: cc_cmd_print <claude> <interval> <heartbeat> <quiet> <life> */
int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s <claude> <interval> <heartbeat> <quiet> <life>\n", argv[0]);
        return 2;
    }
    cc_watch_params_t p = { atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]) };
    char out[2048];
    if (!cc_cmd_watch(argv[1], &p, out, sizeof(out))) {
        fprintf(stderr, "cc_cmd_watch refused the arguments\n");
        return 1;
    }
    printf("%s\n", out);
    return 0;
}
