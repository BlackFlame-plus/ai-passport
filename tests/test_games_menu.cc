#include "apps/games/games_menu.h"
#include <cassert>
#include <cstring>

int main() {
    const games_menu_entry_t *e = games_menu_entries();
    assert(GAMES_MENU_COUNT == 3);
    assert(e[0].id == APP_ID_GAME && std::strcmp(e[0].title, "你在狗叫什么") == 0);
    assert(e[1].id == APP_ID_SOUND_JUMP && std::strcmp(e[1].title, "声音跳一跳") == 0);
    assert(e[2].id == APP_ID_TETRIS && std::strcmp(e[2].title, "俄罗斯方块") == 0);

    assert(games_menu_prev(0) == 2);
    assert(games_menu_next(2) == 0);
    assert(games_menu_next(0) == 1);
    assert(games_menu_prev(1) == 0);
    assert(games_menu_id_at(0) == APP_ID_GAME);
    assert(games_menu_id_at(1) == APP_ID_SOUND_JUMP);
    assert(games_menu_id_at(2) == APP_ID_TETRIS);
    assert(games_menu_id_at(99) == APP_ID_GAME);

    puts("Games menu: order, wrap-around bounds, id mapping PASS");
    return 0;
}
