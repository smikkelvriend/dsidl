// Minimal shim: only the inflate part of miniz is used
#pragma once
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
#include "miniz_common.h"
#include "miniz_tinfl.h"
