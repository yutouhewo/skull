#include <stdlib.h>

#include "flibs/fev.h"
#include "api/sk_const.h"

void* sk_eventloop_create()
{
    return fev_create(SK_EVENTLOOP_MAX_EVENTS);
}
void sk_eventloop_destroy(void* evlp)
{
    fev_destroy(evlp);
}

int sk_eventloop_dispatch(void* evlp, int timeout)
{
    return fev_poll(evlp, timeout);
}
