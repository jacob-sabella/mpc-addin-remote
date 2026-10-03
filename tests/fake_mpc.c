// A stand-in process for the preload smoke test: copied as "MPC" (the add-in starts) and as "other" (it must not).
#include <unistd.h>
int main(void)
{
    sleep(30);
    return 0;
}
