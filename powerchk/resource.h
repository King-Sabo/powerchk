#pragma once

#define IDI_POWERCHK 101

// Version — bump here; the .rc VERSIONINFO (and code, if desired) read these.
#define PCHK_VER_MAJOR 1
#define PCHK_VER_MINOR 0
#define PCHK_VER_PATCH 3
#define PCHK_VER_BUILD 0

#define PCHK_VER_NUM  PCHK_VER_MAJOR, PCHK_VER_MINOR, PCHK_VER_PATCH, PCHK_VER_BUILD
#define PCHK__STR2(x) #x
#define PCHK__STR(x)  PCHK__STR2(x)
#define PCHK_VER_STR  PCHK__STR(PCHK_VER_MAJOR) "." PCHK__STR(PCHK_VER_MINOR) "." \
                      PCHK__STR(PCHK_VER_PATCH) "." PCHK__STR(PCHK_VER_BUILD)
