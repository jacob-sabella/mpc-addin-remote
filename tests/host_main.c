// The addin's server in a test process: started directly (the process isn't MPC), with REMOTE_FAKE_FB and
// REMOTE_FAKE_TOUCH standing in for the display and the touchscreen. Runs until killed.
#include <stdio.h>
#include <unistd.h>
int mpc_remote_addin_start(void);
int main(void)
{
    if (mpc_remote_addin_start()) return 1;
    for (;;) pause();
}
