-- Native InsertService load path for a saved legacy `Hinge` class.
-- Prints the result tree so the deserializer's accept/drop/substitute policy is
-- observed directly rather than inferred from class registration.
local IS = game:GetService("InsertService")

local function describe(v)
    if typeof(v) ~= "Instance" then
        return tostring(v) .. " type=" .. typeof(v)
    end
    return v.ClassName .. " name=" .. v.Name
end

local function dump(root, indent)
    print("LEGACYHINGE tree " .. indent .. describe(root))
    for _, child in ipairs(root:GetDescendants()) do
        print("LEGACYHINGE tree " .. indent .. "  " .. describe(child))
    end
end

for _, url in ipairs({
    "rbxasset://legacy_hinge.rbxmx",
    "rbxasset://content/legacy_hinge.rbxmx",
    "rbxasset://avatar/character.rbxm",
}) do
    local ok, result = pcall(function()
        return IS:LoadLocalAsset(url)
    end)
    print("LEGACYHINGE url=" .. url .. " ok=" .. tostring(ok) .. " result=" .. describe(result))
    if ok and typeof(result) == "Instance" then
        dump(result, "")
        result:Destroy()
    end
end

print("LEGACYHINGE-DONE")
