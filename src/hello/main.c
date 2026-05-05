#define _GNU_SOURCE
#include <stdio.h>

#include "xovi.h"

void _xovi_construct(void) {
    fprintf(stderr, "[cropPaste] hello-world extension loaded\n");
}
