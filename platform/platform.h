/*
 * This file should define the generic types (and possibly header files)
 * needed for building / porting to a different platform.
 *
 * In addition, the video and event layers will also need to be implemented.
 */

#ifndef HAVE_PLATFORM_HEADER
#define HAVE_PLATFORM_HEADER

#define BADFD -1
#include <pthread.h>
#include <semaphore.h>

typedef int pipe_handle;
typedef int file_handle;
typedef pid_t process_handle;
typedef sem_t* sem_handle;

int arcan_sem_post(sem_handle sem);
int arcan_sem_unlink(sem_handle sem, char* key);
int arcan_sem_wait(sem_handle sem);
int arcan_sem_trywait(sem_handle sem);
int arcan_sem_init(sem_handle*, unsigned value);
int arcan_sem_destroy(sem_handle);

typedef int8_t arcan_errc;
typedef long long arcan_vobj_id;
typedef int arcan_aobj_id;

long long int arcan_timemillis();

void arcan_timesleep(unsigned long);
file_handle arcan_fetchhandle(int insock);
bool arcan_pushhandle(file_handle in, int channel);

bool arcan_isdir(const char* path);
bool arcan_isfile(const char* path);

/*
 * implemented in <platform>/warning.c
 * regular fprintf(stderr, style trace output logging.
 * slated for REDESIGN/REFACTOR.
 */
void arcan_warning(const char* msg, ...);
void arcan_fatal(const char* msg, ...);
#endif
