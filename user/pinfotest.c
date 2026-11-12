#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"
#include "kernel/pinfo.h"

struct pinfo buf[NPROC];

int
main(int argc, char *argv[])
{
    int count = getpinfo(buf, NPROC);

    for (int i = 0; i < count; ++i) {
        printf("pid: %d, state: %d, level: %d, runticks: %d, nsched: %d, ctime: %d, name: %s\n",
            buf[i].pid,
            buf[i].state,
            buf[i].level,
            buf[i].runticks,
            buf[i].nsched,
            buf[i].ctime,
            buf[i].name);
    }
    exit(0);
}