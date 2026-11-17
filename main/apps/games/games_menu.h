#pragma once

// Pure navigation helpers for the Games hub (host-testable).

#include "apps/app_base.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { GAMES_MENU_COUNT = 3 };

typedef struct {
    passport_app_id_t id;
    const char *title;
} games_menu_entry_t;

static inline const games_menu_entry_t *games_menu_entries(void) {
    static const games_menu_entry_t kEntries[GAMES_MENU_COUNT] = {
        {APP_ID_GAME, "你在狗叫什么"},
        {APP_ID_SOUND_JUMP, "声音跳一跳"},
        {APP_ID_TETRIS, "俄罗斯方块"},
    };
    return kEntries;
}

static inline size_t games_menu_prev(size_t selected) {
    return (selected + GAMES_MENU_COUNT - 1u) % (size_t)GAMES_MENU_COUNT;
}

static inline size_t games_menu_next(size_t selected) {
    return (selected + 1u) % (size_t)GAMES_MENU_COUNT;
}

static inline passport_app_id_t games_menu_id_at(size_t selected) {
    if (selected >= (size_t)GAMES_MENU_COUNT) selected = 0;
    return games_menu_entries()[selected].id;
}

#ifdef __cplusplus
}
#endif
