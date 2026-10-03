// Runs the addin in a process of its own instead of inside MPC: dlopen the .so and start it. DRM capture and
// touch injection work from any root process, so this tries the addin on a device without restarting MPC.
//   standalone <path/to/mpc_remote_addin.so>      (settings: MPC_REMOTE_ADDIN_CONF or the .conf next to the .so)
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s <mpc_remote_addin.so>\n", argv[0]); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    int (*start)(void) = (int (*)(void))dlsym(h, "mpc_remote_addin_start");
    if (!start || start()) { fprintf(stderr, "the addin didn't start\n"); return 1; }
    for (;;) pause();
}
