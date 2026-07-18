// Give the launched program a controlling terminal (so Steam's inner client
// doesn't re-exec through its bootstrapper and drop our DYLD injection), while
// preserving the environment — unlike /usr/bin/script, which is SIP-protected
// and causes dyld to purge DYLD_* before the child starts.
#include <util.h>
#include <unistd.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    if (pid < 0) return 1;
    if (pid == 0) { execv(argv[1], &argv[1]); _exit(127); }
    char buf[4096];
    while (read(master, buf, sizeof buf) > 0) { /* drain child tty output */ }
    return 0;
}
