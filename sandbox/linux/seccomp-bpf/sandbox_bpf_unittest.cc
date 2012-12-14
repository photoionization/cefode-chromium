// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/prctl.h>
#include <sys/utsname.h>

#include <ostream>

#include "sandbox/linux/seccomp-bpf/bpf_tests.h"
#include "sandbox/linux/seccomp-bpf/verifier.h"
#include "testing/gtest/include/gtest/gtest.h"

using namespace playground2;

namespace {

const int kExpectedReturnValue = 42;

// This test should execute no matter whether we have kernel support. So,
// we make it a TEST() instead of a BPF_TEST().
TEST(SandboxBpf, CallSupports) {
  // We check that we don't crash, but it's ok if the kernel doesn't
  // support it.
  bool seccomp_bpf_supported =
      Sandbox::supportsSeccompSandbox(-1) == Sandbox::STATUS_AVAILABLE;
  // We want to log whether or not seccomp BPF is actually supported
  // since actual test coverage depends on it.
  RecordProperty("SeccompBPFSupported",
                 seccomp_bpf_supported ? "true." : "false.");
  std::cout << "Seccomp BPF supported: "
            << (seccomp_bpf_supported ? "true." : "false.")
            << "\n";
  RecordProperty("PointerSize", sizeof(void*));
  std::cout << "Pointer size: " << sizeof(void*) << "\n";
}

SANDBOX_TEST(SandboxBpf, CallSupportsTwice) {
  Sandbox::supportsSeccompSandbox(-1);
  Sandbox::supportsSeccompSandbox(-1);
}

// BPF_TEST does a lot of the boiler-plate code around setting up a
// policy and optional passing data between the caller, the policy and
// any Trap() handlers. This is great for writing short and concise tests,
// and it helps us accidentally forgetting any of the crucial steps in
// setting up the sandbox. But it wouldn't hurt to have at least one test
// that explicitly walks through all these steps.

intptr_t FakeGetPid(const struct arch_seccomp_data& args, void *aux) {
  BPF_ASSERT(aux);
  pid_t *pid_ptr = static_cast<pid_t *>(aux);
  return (*pid_ptr)++;
}

ErrorCode VerboseAPITestingPolicy(int sysno, void *aux) {
  if (!Sandbox::isValidSyscallNumber(sysno)) {
    return ErrorCode(ENOSYS);
  } else if (sysno == __NR_getpid) {
    return Sandbox::Trap(FakeGetPid, aux);
  } else {
    return ErrorCode(ErrorCode::ERR_ALLOWED);
  }
}

SANDBOX_TEST(SandboxBpf, VerboseAPITesting) {
  if (Sandbox::supportsSeccompSandbox(-1) ==
      playground2::Sandbox::STATUS_AVAILABLE) {
    pid_t test_var = 0;
    playground2::Sandbox::setSandboxPolicy(VerboseAPITestingPolicy, &test_var);
    playground2::Sandbox::startSandbox();

    BPF_ASSERT(test_var == 0);
    BPF_ASSERT(syscall(__NR_getpid) == 0);
    BPF_ASSERT(test_var == 1);
    BPF_ASSERT(syscall(__NR_getpid) == 1);
    BPF_ASSERT(test_var == 2);

    // N.B.: Any future call to getpid() would corrupt the stack.
    //       This is OK. The SANDBOX_TEST() macro is guaranteed to
    //       only ever call _exit() after the test completes.
  }
}

// A simple blacklist test

ErrorCode BlacklistNanosleepPolicy(int sysno, void *) {
  if (!Sandbox::isValidSyscallNumber(sysno)) {
    // FIXME: we should really not have to do that in a trivial policy
    return ErrorCode(ENOSYS);
  }

  switch (sysno) {
    case __NR_nanosleep:
      return ErrorCode(EACCES);
    default:
      return ErrorCode(ErrorCode::ERR_ALLOWED);
  }
}

BPF_TEST(SandboxBpf, ApplyBasicBlacklistPolicy, BlacklistNanosleepPolicy) {
  // nanosleep() should be denied
  const struct timespec ts = {0, 0};
  errno = 0;
  BPF_ASSERT(syscall(__NR_nanosleep, &ts, NULL) == -1);
  BPF_ASSERT(errno == EACCES);
}

// Now do a simple whitelist test

ErrorCode WhitelistGetpidPolicy(int sysno, void *) {
  switch (sysno) {
    case __NR_getpid:
    case __NR_exit_group:
      return ErrorCode(ErrorCode::ERR_ALLOWED);
    default:
      return ErrorCode(ENOMEM);
  }
}

BPF_TEST(SandboxBpf, ApplyBasicWhitelistPolicy, WhitelistGetpidPolicy) {
  // getpid() should be allowed
  errno = 0;
  BPF_ASSERT(syscall(__NR_getpid) > 0);
  BPF_ASSERT(errno == 0);

  // getpgid() should be denied
  BPF_ASSERT(getpgid(0) == -1);
  BPF_ASSERT(errno == ENOMEM);
}

// A simple blacklist policy, with a SIGSYS handler

intptr_t EnomemHandler(const struct arch_seccomp_data& args, void *aux) {
  // We also check that the auxiliary data is correct
  SANDBOX_ASSERT(aux);
  *(static_cast<int*>(aux)) = kExpectedReturnValue;
  return -ENOMEM;
}

ErrorCode BlacklistNanosleepPolicySigsys(int sysno, void *aux) {
  if (!Sandbox::isValidSyscallNumber(sysno)) {
    // FIXME: we should really not have to do that in a trivial policy
    return ErrorCode(ENOSYS);
  }

  switch (sysno) {
    case __NR_nanosleep:
      return Sandbox::Trap(EnomemHandler, aux);
    default:
      return ErrorCode(ErrorCode::ERR_ALLOWED);
  }
}

BPF_TEST(SandboxBpf, BasicBlacklistWithSigsys,
         BlacklistNanosleepPolicySigsys, int /* BPF_AUX */) {
  // getpid() should work properly
  errno = 0;
  BPF_ASSERT(syscall(__NR_getpid) > 0);
  BPF_ASSERT(errno == 0);

  // Our Auxiliary Data, should be reset by the signal handler
  BPF_AUX = -1;
  const struct timespec ts = {0, 0};
  BPF_ASSERT(syscall(__NR_nanosleep, &ts, NULL) == -1);
  BPF_ASSERT(errno == ENOMEM);

  // We expect the signal handler to modify AuxData
  BPF_ASSERT(BPF_AUX == kExpectedReturnValue);
}

// A more complex, but synthetic policy. This tests the correctness of the BPF
// program by iterating through all syscalls and checking for an errno that
// depends on the syscall number. Unlike the Verifier, this exercises the BPF
// interpreter in the kernel.

// We try to make sure we exercise optimizations in the BPF compiler. We make
// sure that the compiler can have an opportunity to coalesce syscalls with
// contiguous numbers and we also make sure that disjoint sets can return the
// same errno.
int SysnoToRandomErrno(int sysno) {
  // Small contiguous sets of 3 system calls return an errno equal to the
  // index of that set + 1 (so that we never return a NUL errno).
  return ((sysno & ~3) >> 2) % 29 + 1;
}

ErrorCode SyntheticPolicy(int sysno, void *) {
  if (!Sandbox::isValidSyscallNumber(sysno)) {
    // FIXME: we should really not have to do that in a trivial policy
    return ErrorCode(ENOSYS);
  }

// TODO(jorgelo): remove this once the new code generator lands.
#if defined(__arm__)
  if (sysno > static_cast<int>(MAX_PUBLIC_SYSCALL)) {
    return ErrorCode(ENOSYS);
  }
#endif

  if (sysno == __NR_exit_group || sysno == __NR_write) {
    // exit_group() is special, we really need it to work.
    // write() is needed for BPF_ASSERT() to report a useful error message.
    return ErrorCode(ErrorCode::ERR_ALLOWED);
  } else {
    return ErrorCode(SysnoToRandomErrno(sysno));
  }
}

BPF_TEST(SandboxBpf, SyntheticPolicy, SyntheticPolicy) {
  // Ensure that that kExpectedReturnValue + syscallnumber + 1 does not int
  // overflow.
  BPF_ASSERT(
   std::numeric_limits<int>::max() - kExpectedReturnValue - 1 >=
   static_cast<int>(MAX_PUBLIC_SYSCALL));

  for (int syscall_number =  static_cast<int>(MIN_SYSCALL);
           syscall_number <= static_cast<int>(MAX_PUBLIC_SYSCALL);
         ++syscall_number) {
    if (syscall_number == __NR_exit_group ||
        syscall_number == __NR_write) {
      // exit_group() is special
      continue;
    }
    errno = 0;
    BPF_ASSERT(syscall(syscall_number) == -1);
    BPF_ASSERT(errno == SysnoToRandomErrno(syscall_number));
  }
}

#if defined(__arm__)
// A simple policy that tests whether ARM private system calls are supported
// by our BPF compiler and by the BPF interpreter in the kernel.

// For ARM private system calls, return an errno equal to their offset from
// MIN_PRIVATE_SYSCALL plus 1 (to avoid NUL errno).
int ArmPrivateSysnoToErrno(int sysno) {
  if (sysno >= static_cast<int>(MIN_PRIVATE_SYSCALL) &&
      sysno <= static_cast<int>(MAX_PRIVATE_SYSCALL)) {
    return (sysno - MIN_PRIVATE_SYSCALL) + 1;
  } else {
    return ENOSYS;
  }
}

ErrorCode ArmPrivatePolicy(int sysno, void *) {
  if (!Sandbox::isValidSyscallNumber(sysno)) {
    // FIXME: we should really not have to do that in a trivial policy.
    return ErrorCode(ENOSYS);
  }

  // Start from |__ARM_NR_set_tls + 1| so as not to mess with actual
  // ARM private system calls.
  if (sysno >= static_cast<int>(__ARM_NR_set_tls + 1) &&
      sysno <= static_cast<int>(MAX_PRIVATE_SYSCALL)) {
    return ErrorCode(ArmPrivateSysnoToErrno(sysno));
  } else {
    return ErrorCode(ErrorCode::ERR_ALLOWED);
  }
}

BPF_TEST(SandboxBpf, ArmPrivatePolicy, ArmPrivatePolicy) {
  for (int syscall_number =  static_cast<int>(__ARM_NR_set_tls + 1);
           syscall_number <= static_cast<int>(MAX_PRIVATE_SYSCALL);
         ++syscall_number) {
    errno = 0;
    BPF_ASSERT(syscall(syscall_number) == -1);
    BPF_ASSERT(errno == ArmPrivateSysnoToErrno(syscall_number));
  }
}
#endif  // defined(__arm__)

intptr_t CountSyscalls(const struct arch_seccomp_data& args, void *aux) {
  // Count all invocations of our callback function.
  ++*reinterpret_cast<int *>(aux);

  // Verify that within the callback function all filtering is temporarily
  // disabled.
  BPF_ASSERT(syscall(__NR_getpid) > 1);

  // Verify that we can now call the underlying system call without causing
  // infinite recursion.
  return Sandbox::ForwardSyscall(args);
}

ErrorCode GreyListedPolicy(int sysno, void *aux) {
  // The use of UnsafeTrap() causes us to print a warning message. This is
  // generally desirable, but it results in the unittest failing, as it doesn't
  // expect any messages on "stderr". So, temporarily disable messages. The
  // BPF_TEST() is guaranteed to turn messages back on, after the policy
  // function has completed.
  Die::SuppressInfoMessages(true);

  // Some system calls must always be allowed, if our policy wants to make
  // use of UnsafeTrap()
  if (sysno == __NR_rt_sigprocmask ||
      sysno == __NR_rt_sigreturn
#if defined(__NR_sigprocmask)
   || sysno == __NR_sigprocmask
#endif
#if defined(__NR_sigreturn)
   || sysno == __NR_sigreturn
#endif
      ) {
    return ErrorCode(ErrorCode::ERR_ALLOWED);
  } else if (sysno == __NR_getpid) {
    // Disallow getpid()
    return ErrorCode(EPERM);
  } else if (Sandbox::isValidSyscallNumber(sysno)) {
    // Allow (and count) all other system calls.
      return Sandbox::UnsafeTrap(CountSyscalls, aux);
  } else {
    return ErrorCode(ENOSYS);
  }
}

BPF_TEST(SandboxBpf, GreyListedPolicy,
         GreyListedPolicy, int /* BPF_AUX */) {
  BPF_ASSERT(syscall(__NR_getpid) == -1);
  BPF_ASSERT(errno == EPERM);
  BPF_ASSERT(BPF_AUX == 0);
  BPF_ASSERT(syscall(__NR_geteuid) == syscall(__NR_getuid));
  BPF_ASSERT(BPF_AUX == 2);
  char name[17] = { };
  BPF_ASSERT(!syscall(__NR_prctl, PR_GET_NAME, name, (void *)NULL,
                      (void *)NULL, (void *)NULL));
  BPF_ASSERT(BPF_AUX == 3);
  BPF_ASSERT(*name);
}

intptr_t PrctlHandler(const struct arch_seccomp_data& args, void *) {
  if (args.args[0] == PR_CAPBSET_DROP &&
      static_cast<int>(args.args[1]) == -1) {
    // prctl(PR_CAPBSET_DROP, -1) is never valid. The kernel will always
    // return an error. But our handler allows this call.
    return 0;
  } else {
    return Sandbox::ForwardSyscall(args);
  }
}

ErrorCode PrctlPolicy(int sysno, void *aux) {
  Die::SuppressInfoMessages(true);

  if (sysno == __NR_prctl) {
    // Handle prctl() inside an UnsafeTrap()
    return Sandbox::UnsafeTrap(PrctlHandler, NULL);
  } else if (Sandbox::isValidSyscallNumber(sysno)) {
    // Allow all other system calls.
    return ErrorCode(ErrorCode::ERR_ALLOWED);
  } else {
    return ErrorCode(ENOSYS);
  }
}

BPF_TEST(SandboxBpf, ForwardSyscall, PrctlPolicy) {
  // This call should never be allowed. But our policy will intercept it and
  // let it pass successfully.
  BPF_ASSERT(!prctl(PR_CAPBSET_DROP, -1, (void *)NULL, (void *)NULL,
                    (void *)NULL));

  // Verify that the call will fail, if it makes it all the way to the kernel.
  BPF_ASSERT(prctl(PR_CAPBSET_DROP, -2, (void *)NULL, (void *)NULL,
                   (void *)NULL) == -1);

  // And verify that other uses of prctl() work just fine.
  char name[17] = { };
  BPF_ASSERT(!syscall(__NR_prctl, PR_GET_NAME, name, (void *)NULL,
                      (void *)NULL, (void *)NULL));
  BPF_ASSERT(*name);

  // Finally, verify that system calls other than prctl() are completely
  // unaffected by our policy.
  struct utsname uts = { };
  BPF_ASSERT(!uname(&uts));
  BPF_ASSERT(!strcmp(uts.sysname, "Linux"));
}

intptr_t AllowRedirectedSyscall(const struct arch_seccomp_data& args, void *) {
  return Sandbox::ForwardSyscall(args);
}

ErrorCode RedirectAllSyscallsPolicy(int sysno, void *aux) {
  Die::SuppressInfoMessages(true);

  // Some system calls must always be allowed, if our policy wants to make
  // use of UnsafeTrap()
  if (sysno == __NR_rt_sigprocmask ||
      sysno == __NR_rt_sigreturn
#if defined(__NR_sigprocmask)
   || sysno == __NR_sigprocmask
#endif
#if defined(__NR_sigreturn)
   || sysno == __NR_sigreturn
#endif
      ) {
    return ErrorCode(ErrorCode::ERR_ALLOWED);
  } else if (Sandbox::isValidSyscallNumber(sysno)) {
    return Sandbox::UnsafeTrap(AllowRedirectedSyscall, aux);
  } else {
    return ErrorCode(ENOSYS);
  }
}

int bus_handler_fd_ = -1;

void SigBusHandler(int, siginfo_t *info, void *void_context) {
  BPF_ASSERT(write(bus_handler_fd_, "\x55", 1) == 1);
}

BPF_TEST(SandboxBpf, SigBus, RedirectAllSyscallsPolicy) {
  // We use the SIGBUS bit in the signal mask as a thread-local boolean
  // value in the implementation of UnsafeTrap(). This is obviously a bit
  // of a hack that could conceivably interfere with code that uses SIGBUS
  // in more traditional ways. This test verifies that basic functionality
  // of SIGBUS is not impacted, but it is certainly possibly to construe
  // more complex uses of signals where our use of the SIGBUS mask is not
  // 100% transparent. This is expected behavior.
  int fds[2];
  BPF_ASSERT(pipe(fds) == 0);
  bus_handler_fd_ = fds[1];
  struct sigaction sa = { };
  sa.sa_sigaction = SigBusHandler;
  sa.sa_flags = SA_SIGINFO;
  BPF_ASSERT(sigaction(SIGBUS, &sa, NULL) == 0);
  raise(SIGBUS);
  char c = '\000';
  BPF_ASSERT(read(fds[0], &c, 1) == 1);
  BPF_ASSERT(close(fds[0]) == 0);
  BPF_ASSERT(close(fds[1]) == 0);
  BPF_ASSERT(c == 0x55);
}

BPF_TEST(SandboxBpf, SigMask, RedirectAllSyscallsPolicy) {
  // Signal masks are potentially tricky to handle. For instance, if we
  // ever tried to update them from inside a Trap() or UnsafeTrap() handler,
  // the call to sigreturn() at the end of the signal handler would undo
  // all of our efforts. So, it makes sense to test that sigprocmask()
  // works, even if we have a policy in place that makes use of UnsafeTrap().
  // In practice, this works because we force sigprocmask() to be handled
  // entirely in the kernel.
  sigset_t mask0, mask1, mask2;

  // Call sigprocmask() to verify that SIGUSR1 wasn't blocked, if we didn't
  // change the mask (it shouldn't have been, as it isn't blocked by default
  // in POSIX).
  sigemptyset(&mask0);
  BPF_ASSERT(!sigprocmask(SIG_BLOCK, &mask0, &mask1));
  BPF_ASSERT(!sigismember(&mask1, SIGUSR1));

  // Try again, and this time we verify that we can block it. This
  // requires a second call to sigprocmask().
  sigaddset(&mask0, SIGUSR1);
  BPF_ASSERT(!sigprocmask(SIG_BLOCK, &mask0, NULL));
  BPF_ASSERT(!sigprocmask(SIG_BLOCK, NULL, &mask2));
  BPF_ASSERT( sigismember(&mask2, SIGUSR1));
}

BPF_TEST(SandboxBpf, UnsafeTrapWithErrno, RedirectAllSyscallsPolicy) {
  // An UnsafeTrap() (or for that matter, a Trap()) has to report error
  // conditions by returning an exit code in the range -1..-4096. This
  // should happen automatically if using ForwardSyscall(). If the TrapFnc()
  // uses some other method to make system calls, then it is responsible
  // for computing the correct return code.
  // This test verifies that ForwardSyscall() does the correct thing.

  // The glibc system wrapper will ultimately set errno for us. So, from normal
  // userspace, all of this should be completely transparent.
  errno = 0;
  BPF_ASSERT(close(-1) == -1);
  BPF_ASSERT(errno == EBADF);

  // Explicitly avoid the glibc wrapper. This is not normally the way anybody
  // would make system calls, but it allows us to verify that we don't
  // accidentally mess with errno, when we shouldn't.
  errno = 0;
  struct arch_seccomp_data args = { };
  args.nr      = __NR_close;
  args.args[0] = -1;
  BPF_ASSERT(Sandbox::ForwardSyscall(args) == -EBADF);
  BPF_ASSERT(errno == 0);
}

} // namespace
