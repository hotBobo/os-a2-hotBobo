#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"
#include "kernel/pinfo.h"

// Part 1 tests for getpinfo. Run with: make CPUS=1 qemu, then `pinfotest`.
// Use `pinfotest -v` to also dump the process table.

#define RUNNING 4 // enum procstate values from kernel/proc.h
#define ZOMBIE 5

struct pinfo buf[NPROC];
int failed = 0;

void
print_pinfo(struct pinfo *p)
{
  printf("  pid %d state %d level %d runticks %d nsched %d ctime %d name %s\n",
         p->pid, p->state, p->level, p->runticks, p->nsched, p->ctime, p->name);
}

// Look up pid and copy its entry into *out. Returns 1 if found, 0 if not.
// Copies by value because every call overwrites the global buf.
int
lookup(int pid, struct pinfo *out)
{
  int count = getpinfo(buf, NPROC);
  for (int i = 0; i < count; i++) {
    if (buf[i].pid == pid) {
      *out = buf[i];
      return 1;
    }
  }
  return 0;
}

void
check(int ok, char *what)
{
  if (ok) {
    printf("OK   %s\n", what);
  } else {
    printf("FAIL %s\n", what);
    failed = 1;
  }
}

// Burn CPU until uptime() has advanced by n ticks.
void
spin(int n)
{
  int start = uptime();
  volatile int x = 0;
  while (uptime() - start < n)
    x++;
}

void
test_basic(int verbose)
{
  printf("-- basic\n");
  int count = getpinfo(buf, NPROC);
  check(count > 0, "getpinfo returns positive count");
  if (verbose)
    for (int i = 0; i < count; i++)
      print_pinfo(&buf[i]);

  struct pinfo me;
  if (!lookup(getpid(), &me)) {
    check(0, "current process is listed");
    return;
  }
  check(1, "current process is listed");
  check(strcmp(me.name, "pinfotest") == 0, "name is pinfotest");
  check(me.state == RUNNING, "state is RUNNING");
  check(me.level >= 0 && me.level <= 3, "level is in 0..3");

  struct pinfo tmp;
  check(lookup(1, &tmp) && strcmp(tmp.name, "init") == 0, "init (pid 1) is listed");
}

void
test_runticks(void)
{
  printf("-- runticks\n");
  struct pinfo before, after;
  lookup(getpid(), &before);
  int t0 = uptime();
  spin(20);
  int t1 = uptime();
  lookup(getpid(), &after);

  int dt = t1 - t0;
  int drt = after.runticks - before.runticks;
  printf("     elapsed %d ticks, charged %d ticks\n", dt, drt);
  check(drt >= dt * 8 / 10, "spinning process is charged ~every tick");
  check(drt <= dt + 1, "not charged more ticks than elapsed");
}

void
test_nsched(void)
{
  printf("-- nsched\n");
  struct pinfo before, after;
  lookup(getpid(), &before);
  for (int i = 0; i < 5; i++)
    pause(1);
  lookup(getpid(), &after);
  printf("     nsched %d -> %d\n", before.nsched, after.nsched);
  check(after.nsched - before.nsched >= 5, "nsched grows by >= 5 after 5 pauses");
}

void
test_child(void)
{
  printf("-- child counters\n");
  spin(10); // make sure the parent has nonzero counters
  struct pinfo parent;
  lookup(getpid(), &parent);

  int pid = fork();
  if (pid < 0) {
    check(0, "fork");
    return;
  }
  if (pid == 0) {
    struct pinfo me;
    if (!lookup(getpid(), &me))
      exit(1);
    printf("     parent runticks %d nsched %d ctime %d | child runticks %d nsched %d ctime %d\n",
           parent.runticks, parent.nsched, parent.ctime, me.runticks, me.nsched, me.ctime);
    int ok = me.runticks <= 1 && me.nsched <= 2 && me.ctime >= parent.ctime;
    exit(ok ? 0 : 1);
  }
  int status = -1;
  wait(&status);
  check(parent.runticks > 1, "parent has nonzero runticks before fork");
  check(status == 0, "child counters start near 0 (not inherited)");
}

void
test_edges(void)
{
  printf("-- edge cases\n");
  check(getpinfo(buf, 0) == 0, "max = 0 returns 0");
  check(getpinfo(buf, -1) == -1, "max = -1 returns -1");
  check(getpinfo(buf, 1) == 1, "max = 1 returns 1");
  check(getpinfo((struct pinfo *)0x3fffffff00L, NPROC) == -1, "bad pointer returns -1");
  check(getpinfo((struct pinfo *)0, NPROC) == -1, "null pointer returns -1");
}

void
test_zombie(void)
{
  printf("-- zombie\n");
  int pid = fork();
  if (pid == 0)
    exit(0);
  pause(5); // let the child exit, but do not wait() yet
  struct pinfo c;
  int found = lookup(pid, &c);
  check(found && c.state == ZOMBIE, "exited child is listed as ZOMBIE before wait");
  wait(0);
  check(!lookup(pid, &c), "child is gone after wait");
}

int
main(int argc, char *argv[])
{
  int verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
  test_basic(verbose);
  test_runticks();
  test_nsched();
  test_child();
  test_edges();
  test_zombie();

  if (failed) {
    printf("pinfotest: SOME TESTS FAILED\n");
    exit(1);
  }
  printf("pinfotest: ALL TESTS PASSED\n");
  exit(0);
}
