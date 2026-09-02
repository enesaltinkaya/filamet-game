#include "GameState.h"

namespace game {
static State state = STATE_MAIN_MENU;

State gameStateCurrent(void) {
    return state;
}

void gameStateSet(State s) {
    state = s;
}
}  // namespace game
