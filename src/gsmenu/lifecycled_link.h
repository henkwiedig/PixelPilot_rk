#pragma once

#include <stdbool.h>

/* Artosyn (AR8030) link state from this ground unit's own ar8030-lifecycled,
 * over its HTTP API (GET http://127.0.0.1:8899/api/v1/link -- the same local
 * address gsmenu.sh's lifecycled_url uses). Plain HTTP over a socket, so no
 * libar8030/libcurl dependency: PixelPilot also runs where neither exists.
 *
 * A background thread polls every 500 ms, started on the first call; call
 * it from the drone-detection timer only in Artosyn mode, so other modes
 * never start the thread. Returns true while lifecycled reports
 * "state":"connected"; false while it reports anything else, is unreachable
 * or its last answer is older than ~2 s. */
bool lifecycled_link_connected(void);
