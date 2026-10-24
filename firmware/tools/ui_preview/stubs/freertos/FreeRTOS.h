#pragma once
/* The preview is one thread: a mutex or a binary semaphore is a token that is never contended. */
#include <stdlib.h>
#define portMAX_DELAY 0xffffffffu
typedef void *SemaphoreHandle_t;
