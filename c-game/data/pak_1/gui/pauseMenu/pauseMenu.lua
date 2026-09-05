-- Port of the old engine's pauseMenu.lua: ESC on the focused pause document
-- returns to the game (the MAIN MENU button is the explicit route to the
-- main menu; pauseExitGame is registered in PauseMenuGui.cpp).
function pauseKeyDown(event)
  local key = event.parameters.key_identifier
  if key == rmlui.key_identifier.ESCAPE then
    pauseReturnToGame()
  end
end
