// The ways a run can end, and the exit code each one leaves behind.
//
// The exit request is process-wide and one-shot: boot_request_exit() records the *first* reason and keeps
// it, because whichever of HalReturnToFirmware, HalInitiateShutdown, KeBugCheck or --strict got there
// first is the reason the run ended. That design is what makes the exit code trustworthy -- and it also
// means these scenarios cannot share one process, so each runs in a forked child. The child inherits the
// already-prepared image, which keeps this suite as fast as the rest.
#include "kernel/k_misc_posix.h"
#include "kernel/ktable_posix.h"
#include "platform/posix/boot.h"
#include "platform/posix/image.h"

#include "xbe_fixture.h"

#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

using namespace bootfix;

static int g_failures, g_checks;

static void check(bool cond, const std::string &what)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        g_failures++;
    }
}

// Returned by a scenario body that did not reach the point it was measuring.
constexpr int DID_NOT_HAPPEN = -1000;

const char *outcome_name(BootOutcome o)
{
    switch (o) {
    case BootOutcome::Returned: return "Returned";
    case BootOutcome::TimedOut: return "TimedOut";
    case BootOutcome::Exited: return "Exited";
    case BootOutcome::FailedToStart: return "FailedToStart";
    }
    return "?";
}

// Run `body` in a child and report whether it saw the outcome and the exit code it expected.
//
// The child reports through a pipe rather than through printf. Everything the loader prints -- the log, the
// stub tail, the boot milestones -- shares this process's stdout and its buffering, so a diagnostic
// printed there can be interleaved into the middle of someone else's line, and a lost diagnostic on a
// flaky failure is worse than no diagnostic at all.
template <typename F> static bool ends_as(BootOutcome want_outcome, int want_code, F body)
{
    int fd[2];
    if (pipe(fd) != 0) {
        check(false, std::string("pipe for the scenario: ") + strerror(errno));
        return false;
    }
    fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        check(false, std::string("fork: ") + strerror(errno));
        return false;
    }
    if (pid == 0) {
        close(fd[0]);
        BootOutcome got = BootOutcome::FailedToStart;
        const int code = body(&got);
        char msg[160];
        snprintf(msg, sizeof msg, "outcome=%d(%s) code=%d", (int)got, outcome_name(got), code);
        ssize_t ignored = write(fd[1], msg, strlen(msg));
        (void)ignored;
        close(fd[1]);
        _exit(0);
    }
    close(fd[1]);
    char msg[160] = "";
    ssize_t got = 0;
    while (got < (ssize_t)sizeof msg - 1) {
        const ssize_t n = read(fd[0], msg + got, sizeof msg - 1 - got);
        if (n <= 0) break;
        got += n;
    }
    close(fd[0]);
    int status = 0;
    const pid_t reaped = waitpid(pid, &status, 0);
    if (reaped != pid) {
        check(false, std::string("waitpid: ") + strerror(errno));
        return false;
    }
    if (WIFSIGNALED(status)) {
        check(false, "the child died from signal " + std::to_string(WTERMSIG(status)));
        return false;
    }
    if (got <= 0) {
        check(false, "the child reported nothing at all");
        return false;
    }
    msg[got] = 0;

    // Now compare, and say what was different -- an unexplained failure cannot be acted on.
    int outcome_code = -1;
    const char *p = strstr(msg, "outcome=");
    if (p) outcome_code = atoi(p + 8);
    const char *c = strstr(msg, "code=");
    const int observed_code = c ? atoi(c + 5) : DID_NOT_HAPPEN;
    if (outcome_code == DID_NOT_HAPPEN || observed_code == DID_NOT_HAPPEN) {
        check(false, std::string("the scenario did not reach what it was measuring (") + msg + ")");
        return false;
    }
    if ((BootOutcome)outcome_code != want_outcome) {
        check(false, std::string("outcome was ") + msg + ", wanted " + outcome_name(want_outcome));
        return false;
    }
    if (observed_code != want_code) {
        check(false, std::string("exit code was ") + msg + ", wanted " + std::to_string(want_code));
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------------------------------------

static void *halt_thread(void *)
{
    HalReturnToFirmware(0); // 0 = halt. Parks: on the console there is nothing left to do.
    return nullptr;
}

static void *bugcheck_thread(void *)
{
    KeBugCheck(0x000000DEu);
    return nullptr;
}

// Map and resolve the fixture again in this process, so a scenario can change which ordinals are implemented
// and run against the new table. Two things make this less obvious than it looks:
//
//   - The image cannot simply be resolved twice. The first pass overwrites every thunk slot with an
//     *address*; a second pass would read those addresses back as ordinals and fail on the first one.
//     So the fixture is mapped again from its bytes, which restores the ordinals.
//   - The image object has to outlive the run. Its destructor unmaps, and the loader maps at 0x10000, so a
//     reload whose image object is a local would leave the game running in an address range that nothing owns
//     -- which is a segfault with no message, and looked for a long time like a bug in the thunk resolver.
static bool reload_fixture()
{
    static XbePosix x;
    static XbeImage img;
    std::string err;
    if (!x.load_from_memory(build(), err)) {
        fprintf(stderr, "reload: %s\n", err.c_str());
        return false;
    }
    img.unmap();
    if (!img.map(x, err) || !img.copy_sections(err)) {
        fprintf(stderr, "reload: %s\n", err.c_str());
        return false;
    }
    img.patch_fs(nullptr);
    if (!kernel_resolve_thunks(x, img, err)) {
        fprintf(stderr, "reload: %s\n", err.c_str());
        return false;
    }
    return true;
}

// Run the fixture's entry point and report the outcome and exit code the program would see.
static int run_and_report(BootOutcome *out)
{
    uint32_t stack_top = 0;
    *out = boot_run_entry(ENTRY_VA, 0, 5000, &stack_top);
    return boot_exit_code();
}

int main()
{
    printf("BLACK loader -- how a run ends\n");

    // Prepare the image once in the parent; every child inherits it mapped, patched and resolved.
    XbePosix x;
    XbeImage img;
    std::string err;
    if (!x.load_from_memory(build(), err) || !img.map(x, err) || !img.copy_sections(err)) {
        check(false, "the fixture mapped: " + err);
        return 1;
    }
    img.patch_fs(nullptr);
    if (!kernel_resolve_thunks(x, img, err)) {
        check(false, "the fixture's imports resolved: " + err);
        return 1;
    }

    // 1. The game asks to leave the ordinary way. HalReturnToFirmware(0) is a halt, so the program exits 0.
    //
    // The request is made *before* the entry point starts, from another thread, because the fixture's entry
    // point returns in microseconds: starting the requesting thread and then the game is a race, and a test
    // that wins or loses it by luck proves nothing. What is being measured here is what the loader does with
    // a request it finds; the request arriving from inside the game is measured by the --strict case below,
    // which is reached from game code by construction.
    check(ends_as(BootOutcome::Exited, 0, [&](BootOutcome *out) {
             pthread_t th;
             if (pthread_create(&th, nullptr, halt_thread, nullptr) != 0) return DID_NOT_HAPPEN;
             for (int i = 0; i < 200 && !boot_exit_requested(); i++) usleep(1000); // 200 ms is plenty
             return run_and_report(out);
         }),
          "HalReturnToFirmware ends the run, and the program exits 0");

    // 2. A bugcheck is not a clean leave, and the exit code has to say so: 4, never 0.
    check(ends_as(BootOutcome::Exited, 4, [&](BootOutcome *out) {
             pthread_t th;
             if (pthread_create(&th, nullptr, bugcheck_thread, nullptr) != 0) return DID_NOT_HAPPEN;
             for (int i = 0; i < 200 && !boot_exit_requested(); i++) usleep(1000);
             return run_and_report(out);
         }),
          "KeBugCheck ends the run with code 4, so a bugcheck never looks like a clean exit");

    // 3. --strict: the first unimplemented kernel call ends the run. Every ordinal is implemented, so the
    //    stub this needs is asked for rather than left in the table: ordinal 137 is held unimplemented for
    //    the length of this check, which puts the stub in the fixture's own call site.
    check(ends_as(BootOutcome::Exited, 2, [&](BootOutcome *out) {
             kernel_hold_unimplemented(137);
             kernel_set_strict(true);
             if (!reload_fixture()) return DID_NOT_HAPPEN;
             const int code = run_and_report(out);
             kernel_release_held();
             // The stub has to be the one the game reached, not a call from here.
             return kernel_stub_calls(137) >= 1 ? code : DID_NOT_HAPPEN;
         }),
          "--strict stops at the first unimplemented call, reached from game code, and exits 2");

    // 4. Without --strict the same call is only logged and the run finishes normally -- which is the whole
    //    point of the flag. boot_exit_code() is -1 because no exit was ever requested.
    check(ends_as(BootOutcome::Returned, -1, [&](BootOutcome *out) {
             kernel_hold_unimplemented(137);
             kernel_set_strict(false);
             if (!reload_fixture()) return DID_NOT_HAPPEN;
             const int code = run_and_report(out);
             kernel_release_held();
             return kernel_stub_calls(137) >= 1 ? code : DID_NOT_HAPPEN;
         }),
          "and without it, no exit is requested at all (code -1, which the program turns into 0)");

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
