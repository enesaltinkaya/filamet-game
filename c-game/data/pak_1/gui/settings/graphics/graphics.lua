local cycleHandlers = {
  first           = { prev = "toggleTaa",           next = "toggleTaa" },
  toggleShadows        = { prev = "toggleShadowsPrev",        next = "toggleShadows" },
  toggleShadowQuality  = { prev = "toggleShadowQualityPrev",  next = "toggleShadowQuality" },
  toggleAo        = { prev = "toggleAo",            next = "toggleAo" },
  toggleSsaoAlgorithm = { prev = "toggleSsaoAlgorithmPrev", next = "toggleSsaoAlgorithm" },
  toggleSss       = { prev = "toggleSss",           next = "toggleSss" },
  toggleBloom     = { prev = "toggleBloom",       next = "toggleBloom" },
  toggleFog       = { prev = "toggleFog",         next = "toggleFog" },
  toggleTaa       = { prev = "toggleTaa",         next = "toggleTaa" },
  toggleFogMode   = { prev = "toggleFogMode",       next = "toggleFogMode" },
}

function graphicsSettingsKeyDown(event)
  local key = event.parameters["key_identifier"]

  -- escape translates to sdl3 gamepad button east, which is "B" in xbox controller
  if key == rmlui.key_identifier.ESCAPE then
    graphicsClose()
    return
  end

  -- Focus/target can be a child element inside the button (label, slider text,
  -- arrow icon, etc.), so walk up until we reach the owning button.
  local element = event.target_element
  while element and element.tag_name ~= "button" do
    element = element.parent_node
  end

  if element then
    local handler = cycleHandlers[element.id]
    if handler then
      if key == rmlui.key_identifier.LEFT then
        _G[handler.prev]()
      elseif key == rmlui.key_identifier.RIGHT then
        _G[handler.next]()
      end
    end
  end
end

-- Slider rows: the arrows are real hit targets (pointer-events: auto), so a
-- click there would otherwise be a dead zone on the slider. Step the row's
-- input by one step instead. SetAttribute fires the range widget's change
-- event (InputTypeRange:OnAttributeChange -> SetValueInternal dispatches it),
-- so the bound float, the label and the deferred apply+persist all run the
-- normal way.
function sliderArrowStep(event)
  local t = event.target_element
  local dir = 1
  while t do
    local class = t.class_name or ""
    if class:find("leftarrow") then
      dir = -1
      break
    elseif class:find("rightarrow") then
      break
    end
    t = t.parent_node
  end

  if not t then return end
  local row = t.parent_node
  if not row then return end
  local input = row:QuerySelector("input")
  if not input then return end

  local min   = tonumber(input:GetAttribute("min")) or 0
  local max   = tonumber(input:GetAttribute("max")) or 0
  local step  = tonumber(input:GetAttribute("step")) or 1
  local value = tonumber(input:GetAttribute("value")) or min
  value = math.max(min, math.min(max, value + dir * step))
  input:SetAttribute("value", value)
end

local cycleDirection = nil

-- RMLUI dispatches the Click event on the row's focus element (the button),
-- never on the arrow glyph itself, so the click target cannot tell the
-- direction. Mousedown does target the glyph, so record the direction there
-- and replay it on the click (default: forward).
function cycleButtonDown(event)
  -- The mousedown target may be the glyph's text node, so walk up to the
  -- first ancestor carrying an arrow class; anything else is "next".
  local t = event.target_element
  cycleDirection = "next"
  while t do
    local class = t.class_name or ""
    if class:find("leftarrow") then
      cycleDirection = "prev"
      break
    elseif class:find("rightarrow") then
      break
    end
    t = t.parent_node
  end
end

function cycleButtonClick(event)
  local element = event.target_element
  while element and element.tag_name ~= "button" do
    element = element.parent_node
  end
  if not element then return end

  local handler = cycleHandlers[element.id]
  if not handler then return end

  local dir = cycleDirection or "next"
  cycleDirection = nil
  _G[handler[dir]]()
end
