-- native_audio_probe.lua — native/Boblox ground truth for Sound spatialization.
--
-- Run through Xecutor in the native client and with --exec on Boblox.  The
-- listener is pinned to the origin and the source is moved on the negative Z
-- axis so both clients see exactly the same distance and listener right axis.
local ws = game:GetService("Workspace")
local service = game:GetService("SoundService")
local camera = ws.CurrentCamera

local function out(s)
    print("AUD " .. s)
    if rconsoleprint then
        rconsoleprint("AUD " .. s .. "\n")
    end
end

if not camera then
    out("NO_CAMERA")
    return
end
camera.CameraType = Enum.CameraType.Scriptable
camera.CFrame = CFrame.new(0, 0, 0)

local sound = Instance.new("Sound")
sound.Name = "AudioProbe"
sound.SoundId = "rbxasset://sounds/electronicpingshort.wav"
local defaultVolume = sound.Volume
local defaultSpeed = sound.PlaybackSpeed
local defaultMin = sound.RollOffMinDistance
local defaultEmitter = sound.EmitterSize
local defaultMax = sound.RollOffMaxDistance
local defaultLooped = sound.Looped
local defaultMode = sound.RollOffMode
out(string.format("DEFAULTS volume=%.6f speed=%.6f min=%.6f emitter=%.6f max=%.6f looped=%s mode=%s",
                  defaultVolume, defaultSpeed, defaultMin, defaultEmitter,
                  defaultMax, tostring(defaultLooped), tostring(defaultMode)))
out(string.format("SERVICE distance=%.6f rolloff=%.6f doppler=%.6f listener=%s",
                  service.DistanceFactor, service.RolloffScale,
                  service.DopplerScale, tostring(service.ListenerType)))
out("LISTENER pos=" ..
    string.format("%.6f,%.6f,%.6f", camera.CFrame.Position.X,
                  camera.CFrame.Position.Y, camera.CFrame.Position.Z))

-- Warm the audio device before the first measured voice.  The native client
-- leaves RollOffGain at zero until the first callback after Play.
do
    local wp = Instance.new("Part")
    wp.Anchored = true
    wp.CanCollide = false
    wp.Transparency = 1
    wp.Size = Vector3.new(1, 1, 1)
    wp.Parent = ws
    local warm = Instance.new("Sound")
    warm.SoundId = "rbxasset://sounds/electronicpingshort.wav"
    warm.Looped = true
    warm.Parent = wp
    warm:Play()
    task.wait(0.5)
    warm:Stop()
    wp:Destroy()
end

local distances = {10, 20, 49.999, 2}
for _, mode in ipairs({Enum.RollOffMode.Inverse, Enum.RollOffMode.Linear,
                       Enum.RollOffMode.LinearSquare,
                       Enum.RollOffMode.InverseTapered}) do
    for _, d in ipairs(distances) do
        local part = Instance.new("Part")
        part.Anchored = true
        part.CanCollide = false
        part.Transparency = 1
        part.Size = Vector3.new(1, 1, 1)
        part.Parent = ws
        local listener = service:GetListenerWorldCFrame()
        part.Position = listener.Position + listener.LookVector * d
        local s = Instance.new("Sound")
        s.SoundId = "rbxasset://sounds/electronicpingshort.wav"
        s.RollOffMode = mode
        s.RollOffMinDistance = 1
        s.EmitterSize = 1
        s.RollOffMaxDistance = 50
        s.Volume = 1
        s.Parent = part
        s:Play()
        -- Some native builds leave RollOffGain at zero for the first
        -- callback after starting a fresh spatial voice.  Poll briefly so a
        -- real zero (the max-distance cutoff) is still reported as zero.
        local gain = s.RollOffGain
        for _ = 1, 12 do
            task.wait(0.05)
            gain = s.RollOffGain
            if gain > 0 then break end
        end
        task.wait(0.05)
        local pos = part.CFrame.Position
        local dir = (pos - service:GetListenerWorldCFrame().Position).Unit
        local right = service:GetListenerWorldCFrame().RightVector
        out(string.format("GAIN mode=%s d=%.3f gain=%.9f dir=%s pan=%.9f",
                          tostring(mode), d, gain,
                          tostring(s.IsSpatial), dir:Dot(right)))
        out(string.format("LOADED mode=%s d=%.3f loaded=%s length=%.6f playing=%s",
                          tostring(mode), d, tostring(s.IsLoaded), s.TimeLength,
                          tostring(s.IsPlaying)))
        s:Stop()
        part:Destroy()
    end
end

for _, side in ipairs({-1, 1}) do
    local base = service:GetListenerWorldCFrame()
    local part = Instance.new("Part")
    part.Anchored = true
    part.CanCollide = false
    part.Transparency = 1
    part.Size = Vector3.new(1, 1, 1)
    part.Parent = ws
    part.Position = base.Position + base.RightVector * (10 * side)
    local s = Instance.new("Sound")
    s.SoundId = "rbxasset://sounds/electronicpingshort.wav"
    s.RollOffMode = Enum.RollOffMode.Inverse
    s.RollOffMinDistance = 1
    s.EmitterSize = 1
    s.RollOffMaxDistance = 50
    s.Volume = 1
    s.Parent = part
    s:Play()
    task.wait(0.25)
    local dir = (part.Position - service:GetListenerWorldCFrame().Position).Unit
    out(string.format("PAN side=%d dot=%.9f", side,
                      dir:Dot(base.RightVector)))
    s:Stop()
    part:Destroy()
end

do
    local base = service:GetListenerWorldCFrame()
    local part = Instance.new("Part")
    part.Anchored = true
    part.CanCollide = false
    part.Transparency = 1
    part.Size = Vector3.new(1, 1, 1)
    part.Parent = ws
    part.Position = base.Position + base.LookVector * 10
    local s = Instance.new("Sound")
    s.SoundId = "rbxasset://sounds/electronicpingshort.wav"
    s.RollOffMode = Enum.RollOffMode.Inverse
    s.RollOffMinDistance = 1
    s.EmitterSize = 5
    s.RollOffMaxDistance = 50
    s.Volume = 1
    s.Parent = part
    s:Play()
    task.wait(0.25)
    out(string.format("EMITTER emitter=5 gain=%.9f", s.RollOffGain))
    s:Stop()
    part:Destroy()
end

local tpPart = Instance.new("Part")
tpPart.Anchored = true
tpPart.CanCollide = false
tpPart.Transparency = 1
tpPart.Size = Vector3.new(1, 1, 1)
tpPart.Parent = ws
local tp = Instance.new("Sound")
tp.SoundId = "rbxasset://sounds/electronicpingshort.wav"
tp.Looped = true
tp.Volume = 1
tp.Parent = tpPart
local group = Instance.new("SoundGroup")
group.Volume = 0.25
group.Parent = service
tp.SoundGroup = group
tp:Play()
task.wait(0.25)
local tp1 = tp.TimePosition
tp.TimePosition = 0.10
task.wait(0.10)
local tp2 = tp.TimePosition
tp.PlaybackSpeed = 2
task.wait(0.15)
local tp3 = tp.TimePosition
tp.Looped = false
tp.TimePosition = 0
task.wait(1.0)
out(string.format("TIME t1=%.6f seek=%.6f fast=%.6f ended=%s length=%.6f group=%.6f",
                  tp1, tp2, tp3, tostring(tp.IsPlaying), tp.TimeLength,
                  group.Volume))
tp:Destroy()
tpPart:Destroy()
group:Destroy()

out("PROBE-DONE")
