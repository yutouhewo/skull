#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <execinfo.h>
#include <sys/types.h>
#include <unistd.h>

#include "api/sk_const.h"
#include "api/sk_utils.h"

void sk_assert_exit(const char* expr, const char* file, int lineno)
{
    printf("FATAL: assert [%s] failed - %s:%d\n", expr, file, lineno);

#ifdef SK_DUMP_CORE
    sk_backtrace_print();
    exit(1);
#else
    abort();
#endif
}

void sk_assert_exit_with_msg(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);

#ifdef SK_DUMP_CORE
    sk_backtrace_print();
    exit(1);
#else
    abort();
#endif
}

void sk_backtrace_print()
{
    void*  buffer[SK_MAX_BACKTRACE];
    int    size;

    size = backtrace(buffer, SK_MAX_BACKTRACE);
    backtrace_symbols_fd(buffer, size, STDERR_FILENO);
}

void sk_util_setup_coreinfo(sk_core_t* core)
{
    // 1. Version
#ifdef SKULL_VERSION
    core->info.version = SKULL_VERSION;
#else
    core->info.version = "unknown";
#endif

    // 2. git sha1
#ifdef SKULL_GIT_SHA1
    core->info.git_sha1 = SKULL_GIT_SHA1;
#else
    core->info.git_sha1 = "unknown";
#endif

    // 3. compiler information
#ifdef __GNUC__
    core->info.compiler = "gcc";
    core->info.compiler_version =
        (SK_EXTRACT_STR(__GNUC__) "."
         SK_EXTRACT_STR(__GNUC_MINOR__) "."
         SK_EXTRACT_STR(__GNUC_PATCHLEVEL__));
#elif defined __clang__
    core->info.compiler = "clang";
    core->info.compiler_version = __clang_version__;
#else
    core->info.compiler = "unknown";
    core->info.compiler_version = "unknown";
#endif

#ifdef SKULL_COMPILER_OPT
    core->info.compiling_options = SKULL_COMPILER_OPT;
#else
    core->info.compiling_options = "unknown";
#endif

    // 4. pid
    core->info.pid = getpid();
}
