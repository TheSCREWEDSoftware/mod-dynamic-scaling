-- Dynamic Scaling (WoW 3.3.5)
-- Talks to mod-dynamic-scaling with addon whispers to yourself (prefix DSCALE).
-- On login it asks which leveling experience you want (No Scaling, Simple or Advanced).
-- /dscale          show or hide the window (your choice, and the creatures around you in each mode)
-- /dscale choose   show the choice again

local PREFIX = "DSCALE"
local ROWS = 12
local ROW_HEIGHT = 20
local MODE_NAMES = { [0] = "No Scaling", [1] = "Simple", [2] = "Advanced" }

local state = {
    mode = nil,          -- 0 off, 1 Simple, 2 Advanced
    canChoose = false,
    offLevels = 5,
    simpleTrail = 3,
    advancedTrail = 2,
    zoneScaled = false,
    advancedMin = 0,
    advancedMax = 0,
    instance = false,
}
local near = {}          -- { entry, name, own, simple, advanced, now, count }
local incoming = {}
local newCharacter = false
local askPending = false

local function Send(body)
    SendAddonMessage(PREFIX, body, "WHISPER", UnitName("player"))
end

local function Print(text)
    DEFAULT_CHAT_FRAME:AddMessage("|cff4CFF00[Dynamic Scaling]|r " .. text)
end

local function Split(text)
    local out = {}
    for piece in string.gmatch(text .. "\t", "(.-)\t") do
        table.insert(out, piece)
    end
    return out
end

local function Backdrop(frame)
    frame:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 32,
        insets = { left = 11, right = 12, top = 12, bottom = 11 },
    })
end

local function MakeMovable(frame)
    frame:SetMovable(true)
    frame:EnableMouse(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", frame.StartMoving)
    frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
end

-- ---------------------------------------------------------------- choice window

local chooser = CreateFrame("Frame", "DynamicScalingChooser", UIParent)
chooser:SetWidth(660); chooser:SetHeight(330)
chooser:SetPoint("CENTER", 0, 60)
chooser:SetFrameStrata("DIALOG")
Backdrop(chooser)
MakeMovable(chooser)
chooser:Hide()
tinsert(UISpecialFrames, "DynamicScalingChooser")

local chooserTitle = chooser:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
chooserTitle:SetPoint("TOP", 0, -20)
chooserTitle:SetText("Choose your leveling experience!")

local chooserSub = chooser:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
chooserSub:SetPoint("TOP", chooserTitle, "BOTTOM", 0, -6)
chooserSub:SetText("You can change this at any time with /dscale.")

local chooserClose = CreateFrame("Button", nil, chooser, "UIPanelCloseButton")
chooserClose:SetPoint("TOPRIGHT", -6, -6)

local options = {}
local function OptionText(mode)
    if mode == 0 then
        local text = "Creatures keep their own levels."
        if state.offLevels and state.offLevels > 0 then
            text = text .. "\n\nCreatures " .. state.offLevels ..
                " or more levels below you give no quest items or progress!"
        end
        return text
    elseif mode == 1 then
        return "Creatures in the open world trail " .. state.simpleTrail ..
            " levels behind you, in any zone, and always give experience."
    end
    return "Creatures trail " .. state.advancedTrail ..
        " levels behind you, but stay inside each zone's level range."
end

for i, mode in ipairs({ 1, 2, 0 }) do
    local option = CreateFrame("Button", nil, chooser)
    option:SetWidth(196); option:SetHeight(190)
    option:SetPoint("TOPLEFT", 22 + (i - 1) * 208, -62)
    option:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    })
    option:SetBackdropColor(0, 0, 0, 0.6)
    option:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight", "ADD")

    option.name = option:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    option.name:SetPoint("TOP", 0, -14)
    option.name:SetText(MODE_NAMES[mode])

    option.current = option:CreateFontString(nil, "OVERLAY", "GameFontGreenSmall")
    option.current:SetPoint("TOP", option.name, "BOTTOM", 0, -2)
    option.current:SetText("(current)")

    option.text = option:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    option.text:SetPoint("TOPLEFT", 12, -52)
    option.text:SetPoint("BOTTOMRIGHT", -12, 12)
    option.text:SetJustifyH("CENTER")
    option.text:SetJustifyV("TOP")

    option.mode = mode
    option:SetScript("OnClick", function(self)
        if not state.canChoose then
            Print("Scaling can't be changed on this server.")
            return
        end
        Send("SET\t" .. self.mode)
        chooser:Hide()
    end)
    options[mode] = option
end

local function MakeCheck(name, label, y)
    local check = CreateFrame("CheckButton", name, chooser, "UICheckButtonTemplate")
    check:SetWidth(24); check:SetHeight(24)
    check:SetPoint("BOTTOMLEFT", 24, y)
    _G[name .. "Text"]:SetText(label)
    return check
end

local dontAsk = MakeCheck("DynamicScalingDontAsk", "Do not ask again on next login", 40)
local dontAskNew = MakeCheck("DynamicScalingDontAskNew", "Do not ask on new characters", 16)
dontAsk:SetScript("OnClick", function(self) DynamicScalingCharDB.dontAsk = self:GetChecked() and true or false end)
dontAskNew:SetScript("OnClick", function(self) DynamicScalingDB.dontAskNew = self:GetChecked() and true or false end)

local function UpdateChooser()
    for mode, option in pairs(options) do
        option.text:SetText(OptionText(mode))
        if state.mode == mode then
            option.current:Show()
            option:SetBackdropBorderColor(0.3, 1, 0.3)
        else
            option.current:Hide()
            option:SetBackdropBorderColor(0.6, 0.6, 0.6)
        end
    end
    dontAsk:SetChecked(DynamicScalingCharDB.dontAsk)
    dontAskNew:SetChecked(DynamicScalingDB.dontAskNew)
end

chooser:SetScript("OnShow", UpdateChooser)

-- ---------------------------------------------------------------- main window

local frame = CreateFrame("Frame", "DynamicScalingFrame", UIParent)
frame:SetWidth(520); frame:SetHeight(400)
frame:SetPoint("CENTER")
Backdrop(frame)
MakeMovable(frame)
frame:Hide()
tinsert(UISpecialFrames, "DynamicScalingFrame")

local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
title:SetPoint("TOP", 0, -18)
title:SetText("Dynamic Scaling")

local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
close:SetPoint("TOPRIGHT", -6, -6)

local modeText = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
modeText:SetPoint("TOPLEFT", 24, -48)

local zoneText = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
zoneText:SetPoint("TOPLEFT", modeText, "BOTTOMLEFT", 0, -6)

local change = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
change:SetWidth(90); change:SetHeight(22)
change:SetPoint("TOPRIGHT", -24, -44)
change:SetText("Change")
change:SetScript("OnClick", function() chooser:Show() end)

local refresh = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
refresh:SetWidth(90); refresh:SetHeight(22)
refresh:SetPoint("TOPRIGHT", change, "BOTTOMRIGHT", 0, -4)
refresh:SetText("Refresh")
refresh:SetScript("OnClick", function() Send("NEAR") end)

local COLUMNS = {
    { "Creature", 24, 200, "LEFT" },
    { "Default", 230, 60, "CENTER" },
    { "Simple", 292, 60, "CENTER" },
    { "Advanced", 354, 66, "CENTER" },
    { "Now", 422, 50, "CENTER" },
}

for _, column in ipairs(COLUMNS) do
    local header = frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    header:SetPoint("TOPLEFT", column[2], -104)
    header:SetWidth(column[3])
    header:SetJustifyH(column[4])
    header:SetText(column[1])
end

local rows = {}
for i = 1, ROWS do
    local row = {}
    for c, column in ipairs(COLUMNS) do
        local text = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        text:SetPoint("TOPLEFT", column[2], -104 - i * ROW_HEIGHT)
        text:SetWidth(column[3])
        text:SetJustifyH(column[4])
        row[c] = text
    end
    rows[i] = row
end

local empty = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
empty:SetPoint("TOP", 0, -150)

local scroll = CreateFrame("ScrollFrame", "DynamicScalingScroll", frame, "FauxScrollFrameTemplate")
scroll:SetPoint("TOPLEFT", 20, -120)
scroll:SetPoint("BOTTOMRIGHT", -40, 30)

local function LevelText(level, highlight)
    local text = (level and level > 0) and tostring(level) or "-"
    if highlight then return "|cff4CFF00" .. text .. "|r" end
    return text
end

local function UpdateRows()
    FauxScrollFrame_Update(scroll, #near, ROWS, ROW_HEIGHT)
    local offset = FauxScrollFrame_GetOffset(scroll)
    for i = 1, ROWS do
        local line = near[i + offset]
        local row = rows[i]
        if line then
            local name = line.name
            if line.count > 1 then name = name .. " (x" .. line.count .. ")" end
            row[1]:SetText(name)
            row[2]:SetText(LevelText(line.own, state.mode == 0))
            row[3]:SetText(LevelText(line.simple, state.mode == 1))
            row[4]:SetText(LevelText(line.advanced, state.mode == 2))
            row[5]:SetText(LevelText(line.now))
        else
            for c = 1, #COLUMNS do row[c]:SetText("") end
        end
    end
    if #near == 0 then
        empty:SetText(state.instance and "Instances use the instance settings." or "No creatures around you.")
    else
        empty:SetText("")
    end
end

scroll:SetScript("OnVerticalScroll", function(self, offset)
    FauxScrollFrame_OnVerticalScroll(self, offset, ROW_HEIGHT, UpdateRows)
end)

local function UpdateFrame()
    modeText:SetText("Your choice: |cff4CFF00" .. (MODE_NAMES[state.mode] or "?") .. "|r")
    if state.instance then
        zoneText:SetText("In an instance: the instance settings apply.")
    elseif state.zoneScaled then
        zoneText:SetText("This place scales. Advanced range: " .. state.advancedMin .. "-" .. state.advancedMax ..
            ". \"-\" means not scaled.")
    else
        zoneText:SetText("This place doesn't scale.")
    end
    if state.canChoose then change:Enable() else change:Disable() end
    UpdateRows()
end

frame:SetScript("OnShow", function()
    Send("HELLO")
    Send("NEAR")
    UpdateFrame()
end)

-- ---------------------------------------------------------------- messages

local function ShouldAsk()
    if not state.canChoose then return false end
    if DynamicScalingCharDB.dontAsk then return false end
    if newCharacter and DynamicScalingDB.dontAskNew then return false end
    return true
end

local function OnMessage(message)
    local parts = Split(message)
    local kind = parts[1]
    if kind == "M" then
        state.mode = tonumber(parts[2])
        state.canChoose = parts[3] == "1"
        state.offLevels = tonumber(parts[4]) or 0
        state.simpleTrail = tonumber(parts[5]) or 3
        state.advancedTrail = tonumber(parts[6]) or 2
    elseif kind == "Z" then
        state.zoneScaled = parts[4] == "1"
        state.advancedMin = tonumber(parts[5]) or 0
        state.advancedMax = tonumber(parts[6]) or 0
        state.instance = parts[7] == "1"
    elseif kind == "N" then
        table.insert(incoming, {
            entry = tonumber(parts[2]), name = parts[3], own = tonumber(parts[4]), simple = tonumber(parts[5]),
            advanced = tonumber(parts[6]), now = tonumber(parts[7]), count = tonumber(parts[8]) or 1,
        })
    elseif kind == "E" then
        if parts[2] == "NEAR" then
            near = incoming
            incoming = {}
        elseif parts[2] == "HELLO" and askPending then
            askPending = false
            if ShouldAsk() then chooser:Show() end
        elseif parts[2] == "SET" then
            Print("Your scaling is now " .. (MODE_NAMES[state.mode] or "?") .. ".")
            if frame:IsShown() then Send("NEAR") end
        end
        if chooser:IsShown() then UpdateChooser() end
        if frame:IsShown() then UpdateFrame() end
    elseif kind == "X" then
        Print(parts[2] or "")
        if parts[2] and parts[2]:find("off on this server") then askPending = false end
    end
end

local events = CreateFrame("Frame")
events:RegisterEvent("ADDON_LOADED")
events:RegisterEvent("PLAYER_LOGIN")
events:RegisterEvent("CHAT_MSG_ADDON")

local loginDelay
events:SetScript("OnEvent", function(self, event, arg1, message, channel, sender)
    if event == "CHAT_MSG_ADDON" then
        if arg1 == PREFIX and channel == "WHISPER" and sender == UnitName("player") then
            OnMessage(message)
        end
    elseif event == "ADDON_LOADED" and arg1 == "DynamicScaling" then
        DynamicScalingDB = DynamicScalingDB or {}
        if not DynamicScalingCharDB then
            newCharacter = true
            DynamicScalingCharDB = {}
        end
    elseif event == "PLAYER_LOGIN" then
        loginDelay = 3 -- seconds, so the server has finished logging us in
    end
end)

events:SetScript("OnUpdate", function(self, elapsed)
    if not loginDelay then return end
    loginDelay = loginDelay - elapsed
    if loginDelay <= 0 then
        loginDelay = nil
        askPending = true
        Send("HELLO")
    end
end)

SLASH_DSCALE1 = "/dscale"
SLASH_DSCALE2 = "/scaling"
SlashCmdList["DSCALE"] = function(arg)
    arg = arg and arg:gsub("^%s+", ""):gsub("%s+$", ""):lower() or ""
    if arg == "choose" then
        Send("HELLO")
        chooser:Show()
    elseif frame:IsShown() then
        frame:Hide()
    else
        frame:Show()
    end
end
