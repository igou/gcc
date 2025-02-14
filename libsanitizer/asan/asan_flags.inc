//===-- asan_flags.inc ------------------------------------------*- C++ -*-===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// ASan runtime flags.
//
//===----------------------------------------------------------------------===//
#ifndef ASAN_FLAG
# error "Define ASAN_FLAG prior to including this file!"
#endif

// ASAN_FLAG(Type, Name, DefaultValue, Description)
// See COMMON_FLAG in sanitizer_flags.inc for more details.

ASAN_FLAG(int, quarantine_size, -1,
            "Deprecated, please use quarantine_size_mb.")
ASAN_FLAG(int, quarantine_size_mb, -1,
          "Size (in Mb) of quarantine used to detect use-after-free "
          "errors. Lower value may reduce memory usage but increase the "
          "chance of false negatives.")
ASAN_FLAG(int, redzone, 16,
          "Minimal size (in bytes) of redzones around heap objects. "
          "Requirement: redzone >= 16, is a power of two.")
ASAN_FLAG(int, max_redzone, 2048,
          "Maximal size (in bytes) of redzones around heap objects.")
ASAN_FLAG(
    bool, debug, false,
    "If set, prints some debugging information and does additional checks.")
ASAN_FLAG(
    int, report_globals, 1,
    "Controls the way to handle globals (0 - don't detect buffer overflow on "
    "globals, 1 - detect buffer overflow, 2 - print data about registered "
    "globals).")
ASAN_FLAG(bool, check_initialization_order, false,
          "If set, attempts to catch initialization order issues.")
ASAN_FLAG(
    bool, replace_str, true,
    "If set, uses custom wrappers and replacements for libc string functions "
    "to find more errors.")
ASAN_FLAG(bool, replace_intrin, true,
          "If set, uses custom wrappers for memset/memcpy/memmove intinsics.")
ASAN_FLAG(bool, mac_ignore_invalid_free, false,
          "Ignore invalid free() calls to work around some bugs. Used on OS X "
          "only.")
ASAN_FLAG(bool, detect_stack_use_after_return, false,
          "Enables stack-use-after-return checking at run-time.")
ASAN_FLAG(int, min_uar_stack_size_log, 16, // We can't do smaller anyway.
          "Minimum fake stack size log.")
ASAN_FLAG(int, max_uar_stack_size_log,
          20, // 1Mb per size class, i.e. ~11Mb per thread
          "Maximum fake stack size log.")
ASAN_FLAG(bool, uar_noreserve, false,
          "Use mmap with 'noreserve' flag to allocate fake stack.")
ASAN_FLAG(
    int, max_malloc_fill_size, 0x1000,  // By default, fill only the first 4K.
    "ASan allocator flag. max_malloc_fill_size is the maximal amount of "
    "bytes that will be filled with malloc_fill_byte on malloc.")
ASAN_FLAG(int, malloc_fill_byte, 0xbe,
          "Value used to fill the newly allocated memory.")
ASAN_FLAG(bool, allow_user_poisoning, true,
          "If set, user may manually mark memory regions as poisoned or "
          "unpoisoned.")
ASAN_FLAG(
    int, sleep_before_dying, 0,
    "Number of seconds to sleep between printing an error report and "
    "terminating the program. Useful for debugging purposes (e.g. when one "
    "needs to attach gdb).")
ASAN_FLAG(bool, check_malloc_usable_size, true,
          "Allows the users to work around the bug in Nvidia drivers prior to "
          "295.*.")
ASAN_FLAG(bool, unmap_shadow_on_exit, false,
          "If set, explicitly unmaps the (huge) shadow at exit.")
ASAN_FLAG(bool, print_stats, false,
          "Print various statistics after printing an error message or if "
          "atexit=1.")
ASAN_FLAG(bool, print_legend, true, "Print the legend for the shadow bytes.")
ASAN_FLAG(bool, atexit, false,
          "If set, prints ASan exit stats even after program terminates "
          "successfully.")
ASAN_FLAG(
    bool, print_full_thread_history, true,
    "If set, prints thread creation stacks for the threads involved in the "
    "report and their ancestors up to the main thread.")
ASAN_FLAG(
    bool, poison_heap, true,
    "Poison (or not) the heap memory on [de]allocation. Zero value is useful "
    "for benchmarking the allocator or instrumentator.")
ASAN_FLAG(bool, poison_partial, true,
          "If true, poison partially addressable 8-byte aligned words "
          "(default=true). This flag affects heap and global buffers, but not "
          "stack buffers.")
ASAN_FLAG(bool, poison_array_cookie, true,
          "Poison (or not) the array cookie after operator new[].")

// Turn off alloc/dealloc mismatch checker on Mac and Windows for now.
// https://code.google.com/p/address-sanitizer/issues/detail?id=131
// https://code.google.com/p/address-sanitizer/issues/detail?id=309
// TODO(glider,timurrrr): Fix known issues and enable this back.
ASAN_FLAG(bool, alloc_dealloc_mismatch,
          (SANITIZER_MAC == 0) && (SANITIZER_WINDOWS == 0),
          "Report errors on malloc/delete, new/free, new/delete[], etc.")

ASAN_FLAG(bool, new_delete_type_mismatch, true,
          "Report errors on mismatch betwen size of new and delete.")
ASAN_FLAG(
    bool, strict_init_order, false,
    "If true, assume that dynamic initializers can never access globals from "
    "other modules, even if the latter are already initialized.")
ASAN_FLAG(
    bool, start_deactivated, false,
    "If true, ASan tweaks a bunch of other flags (quarantine, redzone, heap "
    "poisoning) to reduce memory consumption as much as possible, and "
    "restores them to original values when the first instrumented module is "
    "loaded into the process. This is mainly intended to be used on "
    "Android. ")
ASAN_FLAG(
    int, detect_invalid_pointer_pairs, 0,
    "If non-zero, try to detect operations like <, <=, >, >= and - on "
    "invalid pointer pairs (e.g. when pointers belong to different objects). "
    "The bigger the value the harder we try.")
ASAN_FLAG(
    bool, detect_container_overflow, true,
    "If true, honor the container overflow  annotations. "
    "See https://code.google.com/p/address-sanitizer/wiki/ContainerOverflow")
ASAN_FLAG(int, detect_odr_violation, 2,
          "If >=2, detect violation of One-Definition-Rule (ODR); "
          "If ==1, detect ODR-violation only if the two variables "
          "have different sizes")
ASAN_FLAG(bool, dump_instruction_bytes, false,
          "If true, dump 16 bytes starting at the instruction that caused SEGV")
ASAN_FLAG(const char *, suppressions, "", "Suppressions file name.")
