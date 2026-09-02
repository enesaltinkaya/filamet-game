#pragma once

namespace game {
// Minimal application state machine. Starts in the main menu; more states
// (loading, gameplay sub-states, ...) hang off this as the old UI is ported.
enum State {
    STATE_MAIN_MENU,
    STATE_PLAYING,
};

State gameStateCurrent(void);
void gameStateSet(State state);
}  // namespace game
