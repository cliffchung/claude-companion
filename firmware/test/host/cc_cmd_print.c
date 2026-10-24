#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cc_cmd.h"

/* Prints a command line, for firmware/tools/test_watch_loop.py and test_transcribe_cmd.py.
   usage: cc_cmd_print <claude> <interval> <heartbeat> <quiet> <life>      (cc_cmd_watch)
          cc_cmd_print transcribe <whisper> <model>                        (cc_cmd_transcribe)
          cc_cmd_print voicecheck <whisper> <model>                        (cc_cmd_voice_check) */
int main(int argc, char **argv)
{
    char out[2048];
    if (argc == 4 && (!strcmp(argv[1], "transcribe") || !strcmp(argv[1], "voicecheck"))) {
        bool ok = !strcmp(argv[1], "transcribe") ? cc_cmd_transcribe(argv[2], argv[3], out, sizeof(out))
                                                 : cc_cmd_voice_check(argv[2], argv[3], out, sizeof(out));
        if (!ok) {
            fprintf(stderr, "%s refused the arguments\n", argv[1]);
            return 1;
        }
        printf("%s\n", out);
        return 0;
    }
    if (argc != 6) {
        fprintf(stderr, "usage: %s <claude> <interval> <heartbeat> <quiet> <life>\n"
                        "       %s transcribe|voicecheck <whisper> <model>\n", argv[0], argv[0]);
        return 2;
    }
    cc_watch_params_t p = { atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]) };
    if (!cc_cmd_watch(argv[1], &p, out, sizeof(out))) {
        fprintf(stderr, "cc_cmd_watch refused the arguments\n");
        return 1;
    }
    printf("%s\n", out);
    return 0;
}
