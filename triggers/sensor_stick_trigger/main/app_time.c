#include "app_time.h"

#include <sys/time.h>

#include "ntp_manager.h"

int64_t app_time_epoch_ms(void)
{
    if (!ntp_manager_is_synced()) {
        return 0;
    }

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
