#pragma once

#include <usr/vex/wait.h>

#define WNOHANG    __k_WNOHANG
#define WUNTRACED  __k_WUNTRACED
#define WCONTINUED __k_WCONTINUED

#define W_EXITCODE __k_W_EXITCODE

#define WEXITSTATUS __k_WEXITSTATUS
#define WTERMSIG    __k_WTERMSIG
#define WIFEXITED   __k_WIFEXITED
#define WIFSIGNALED __k_WIFSIGNALED

#define W_STOPCODE  __k_W_STOPCODE
#define W_CONTINUED __k_W_CONTINUED

#define WIFSTOPPED   __k_WIFSTOPPED
#define WSTOPSIG     __k_WSTOPSIG
#define WIFCONTINUED __k_WIFCONTINUED
