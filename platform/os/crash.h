#pragma once

// Install process handlers before boot or switching to the game's low stack.
void port_install_crash_handlers();
// Alternate signal stacks are per host thread on POSIX.
void port_crash_thread_init();
#ifdef _WIN32
void port_install_windows_exception_logger();
#endif
