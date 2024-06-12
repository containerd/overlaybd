#pragma once

#define MACROTOSTR(x) #x
#define PRINTMACRO(x) MACROTOSTR(x)
static const char OVERLAYBD_VERSION[] = PRINTMACRO(OVERLAYBD_VER);
