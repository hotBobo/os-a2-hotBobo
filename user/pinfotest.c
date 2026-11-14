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

// ---------------- Part 2: MLFQ ----------------
// These run in a fresh child (starts at level 0) and report via exit status.
// They assume CPUS=1 and that nothing else is runnable.

// Sleep until we are early in a 64-tick boost period, so a boost
// does not land in the middle of a measurement.
void
avoid_boost(void)
{
  int t;
  while ((t = uptime() % 64) == 0 || t > 20)
    pause(1);
}

// Spin (polling our own level) until we reach `target` level or `limit`
// ticks pass. Returns our pinfo at that moment in *me.
void
spin_until_level(int target, int limit, struct pinfo *me)
{
  int start = uptime();
  lookup(getpid(), me);
  while (me->level < target && uptime() - start < limit)
    lookup(getpid(), me);
}

void
run_in_child(void (*fn)(void), char *what)
{
  int pid = fork();
  if (pid < 0) {
    check(0, "fork");
    return;
  }
  if (pid == 0) {
    fn();
    exit(0); // fn exits with 1 on failure
  }
  int status = -1;
  wait(&status);
  check(status == 0, what);
}

// Rule 5: slices 1,2,4 -> reach levels 1,2,3 after 1,3,7 charged ticks.
// At level 3 the slice is 8, so ~24 ticks of spinning gives ~3 yields.
void
child_demote(void)
{
  avoid_boost();
  struct pinfo me;
  lookup(getpid(), &me);
  int r0 = me.runticks;
  int first[4] = {0, -1, -1, -1};
  int cur = me.level;
  if (cur != 0) {
    printf("     child did not start at level 0 (level %d)\n", cur);
    exit(1);
  }
  int start = uptime();
  while (cur < 3 && uptime() - start < 30) {
    lookup(getpid(), &me);
    while (cur < me.level) {
      cur++;
      first[cur] = me.runticks - r0;
    }
  }
  printf("     reached level 1/2/3 after %d/%d/%d ticks (expect 1/3/7)\n",
         first[1], first[2], first[3]);
  int ok = first[1] >= 1 && first[1] <= 2 &&
           first[2] >= 3 && first[2] <= 4 &&
           first[3] >= 7 && first[3] <= 8;

  // Level 3: spin 24 ticks, should be switched to ~3 times, not ~24.
  int n0 = me.nsched;
  spin(24);
  lookup(getpid(), &me);
  printf("     level %d, nsched +%d over 24 ticks at level 3 (expect ~3)\n",
         me.level, me.nsched - n0);
  ok = ok && me.level == 3 && me.nsched - n0 <= 6;
  exit(ok ? 0 : 1);
}

// A process that only sleeps is never charged, so it stays at level 0.
void
child_sleeper(void)
{
  avoid_boost();
  for (int i = 0; i < 10; i++)
    pause(1);
  struct pinfo me;
  lookup(getpid(), &me);
  printf("     sleeper level %d runticks %d\n", me.level, me.runticks);
  exit(me.level == 0 ? 0 : 1);
}

// Rule 6: run ~1 tick, sleep, repeat. Used time at level 2 must
// accumulate across sleeps, so demotion to 3 happens after ~4 ticks.
void
child_not_forgiven(void)
{
  avoid_boost();
  struct pinfo me;
  spin_until_level(2, 10, &me);
  if (me.level != 2) {
    printf("     never reached level 2 (level %d)\n", me.level);
    exit(1);
  }
  int r2 = me.runticks;
  for (int i = 0; i < 15 && me.level == 2; i++) {
    spin(1);
    pause(1);
    lookup(getpid(), &me);
  }
  printf("     level %d after %d ticks at level 2 split by sleeps (expect 3 after ~4)\n",
         me.level, me.runticks - r2);
  int used = me.runticks - r2;
  exit(me.level == 3 && used >= 4 && used <= 5 ? 0 : 1);
}

// Rule 8: when ticks hits a multiple of 64 everyone goes back to level 0.
void
child_boost(void)
{
  avoid_boost();
  struct pinfo me;
  spin_until_level(3, 15, &me);
  if (me.level != 3) {
    printf("     never reached level 3 (level %d)\n", me.level);
    exit(1);
  }
  int period = uptime() / 64;
  while (uptime() / 64 == period) // spin until the boost tick
    ;
  lookup(getpid(), &me);
  printf("     level %d right after tick %d (expect 0 or 1)\n", me.level, uptime());
  exit(me.level <= 1 ? 0 : 1);
}

void
test_mlfq(void)
{
  printf("-- mlfq (run with CPUS=1)\n");
  run_in_child(child_demote, "demotion after 1/2/4 ticks, level 3 keeps 8-tick slice");
  run_in_child(child_sleeper, "sleeping process stays at level 0");
  run_in_child(child_not_forgiven, "time at a level is not forgiven by sleeping");
  run_in_child(child_boost, "boost every 64 ticks resets level");
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
  test_mlfq();

  if (failed) {
    printf("pinfotest: SOME TESTS FAILED\n");
    exit(1);
  }
  printf("pinfotest: ALL TESTS PASSED\n");
  exit(0);
}
