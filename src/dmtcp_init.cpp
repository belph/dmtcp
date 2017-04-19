// Modified version of https://github.com/krallin/tini
#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>

#include "../jalib/jassert.h"
#include "../jalib/jconvert.h"

using namespace dmtcp;

typedef struct {
  sigset_t* const sigmask_ptr;
  struct sigaction* const sigttin_action_ptr;
  struct sigaction* const sigttou_action_ptr;
} signal_configuration_t;

static struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };

int
restore_signals(const signal_configuration_t* const sigconf_ptr)
{
  JASSERT(!sigprocmask(SIG_SETMASK, sigconf_ptr->sigmask_ptr, NULL));
  JASSERT(!sigaction(SIGTTIN, sigconf_ptr->sigttin_action_ptr, NULL));
  JASSERT(!sigaction(SIGTTOU, sigconf_ptr->sigttou_action_ptr, NULL));
  return 0;
}


int
configure_signals(sigset_t *const parent_sigset_ptr,
                  const signal_configuration_t *const sigconf_ptr)
{
  JASSERT(!sigemptyset(parent_sigset_ptr));
  JASSERT(!sigaddset(parent_sigset_ptr, SIGCHLD));

  JASSERT(!sigprocmask(SIG_SETMASK,
                       parent_sigset_ptr,
                       sigconf_ptr->sigmask_ptr));

  return 0;
}

int
wait_and_forward_signal(sigset_t const* const parent_sigset_ptr,
                        pid_t const child_pid)
{
  siginfo_t sig;

  if (sigtimedwait(parent_sigset_ptr, &sig, &ts) == -1) {
    switch (errno) {
    case EAGAIN:
      break;
    case EINTR:
      break;
    default:
      JASSERT(false) ("Unexpected error in sigtimedwait") (strerror(errno));
      return 1;
    }
  } else {
    /* There is a signal to handle here */
    switch (sig.si_signo) {
    case SIGCHLD:
      /* Special-cased, as we don't forward SIGCHLD. Instead, we'll
       * fallthrough to reaping processes.
       */
      JNOTE("Received SIGCHLD");
      break;
    default:
      JASSERT(false) ("unknown signal")
        (strsignal(sig.si_signo)); // Shouldn't get here
      break;
    }
  }

  return 0;
}

int
reap_zombies(const pid_t child_pid, int* const child_exitcode_ptr)
{
  pid_t current_pid;
  int current_status;

  while (1) {
    current_pid = waitpid(-1, &current_status, WNOHANG);

    switch (current_pid) {
    case -1:
      if (errno == ECHILD) {
        JTRACE("No child to wait");
        break;
      }
      JASSERT(false) ("Error while waiting for pids") (strerror(errno));
      return 1;

    case 0:
      JTRACE("No child to reap");
      break;

    default:
      /* A child was reaped. Check whether it's the main one. If it is, then
       * set the exit_code, which will cause us to exit once we've reaped everyone else.
       */
      JNOTE("Reaped child with pid") (current_pid);

      // Check if other childs have been reaped.
      continue;
    }

    /* If we make it here, that's because we did not
       continue in the switch case. */
    break;
  }

  return 0;
}

int
dmtcp_init_main()
{
  pid_t child_pid = -1;

  // Those are passed to functions to get an exitcode back.
  int child_exitcode = -1;
  // This isn't a valid exitcode, and lets us tell
  // whether the child has exited.


  /* Configure signals */
  sigset_t parent_sigset, child_sigset;
  struct sigaction sigttin_action, sigttou_action;
  memset(&sigttin_action, 0, sizeof sigttin_action);
  memset(&sigttou_action, 0, sizeof sigttou_action);

  signal_configuration_t child_sigconf = {
                                          .sigmask_ptr = &child_sigset,
                                          .sigttin_action_ptr = &sigttin_action,
                                          .sigttou_action_ptr = &sigttou_action,
  };

  if (configure_signals(&parent_sigset, &child_sigconf)) {
    return 1;
  }


  while (1) {
    /* Wait for one signal, and forward it */
    if (wait_and_forward_signal(&parent_sigset, child_pid)) {
      return 1;
    }

    /* Now, reap zombies */
    if (reap_zombies(child_pid, &child_exitcode)) {
      return 1;
    }

    if (child_exitcode != -1) {
      JNOTE("Exiting: child has exited");
      return child_exitcode;
    }
  }
}
