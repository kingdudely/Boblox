-- challenge_src.lua — decompiled 0x9B challenge program (server-personalized constants).
-- Structure identical across servers; only the v5 constants change.
-- Args: (u1, u2) from the challenge message. Returns the answer (as a double).
local v1, v2 = ...

xv = v1
yv = v2

local v3 = { v = xv }
local v4 = { v = yv }
local v5 = {}

v5[1] = __C1__
v5[2] = __C2__
v5[3] = __C3__
v5[4] = __C4__

local v6 = Random.new(xv + yv)

local function f1(v7, v8)
    local v9 = { v = v5[4] }
    local v10 = v9.v + (v7.v + v8.v)
    v9.v = bit32.bor(v10, 0)
    local v11 = v5[1] + (v9.v - v6:NextInteger(1, 2147483647))
    v5[1] = bit32.bor(v11, 0)
    local v12 = v5[4]
    local v13 = v5[2]
    v5[2] = v12
    v5[4] = v13
    local v14 = v5[3]
    local v15 = v5[1]
    v5[1] = v14
    v5[3] = v15
    local v16 = getmetatable(v7)
    setmetatable(v9, v16)
    return v9
end

local function f2(v17, v18)
    local v19 = { v = v5[2] }
    local v20 = v19.v - (v17.v - v18.v)
    v19.v = bit32.bor(v20, 0)
    local v21 = v5[1] - (v19.v + v6:NextInteger(1, 2147483647))
    v5[1] = bit32.bor(v21, 0)
    local v22 = v5[4]
    local v23 = v5[3]
    v5[3] = v22
    v5[4] = v23
    local v24 = v5[2]
    local v25 = v5[1]
    v5[1] = v24
    v5[2] = v25
    local v26 = getmetatable(v17)
    setmetatable(v19, v26)
    return v19
end

local function f3(v27, v28)
    local v29 = { v = v5[3] }
    local v30 = v29.v - (v27.v + v28.v)
    v29.v = bit32.bor(v30, 0)
    local v31 = v5[1] - (v29.v + v6:NextInteger(1, 2147483647))
    v5[1] = bit32.bor(v31, 0)
    local v32 = v5[1]
    local v33 = v5[1]
    v5[1] = v32
    v5[1] = v33
    local v34 = v5[1]
    local v35 = v5[1]
    v5[1] = v34
    v5[1] = v35
    local v36 = getmetatable(v27)
    setmetatable(v29, v36)
    return v29
end

local function f4(v37, v38)
    local v39 = { v = v5[1] }
    local v40 = v39.v + (v37.v + v38.v)
    v39.v = bit32.bor(v40, 0)
    local v41 = v5[1] - (v39.v + v6:NextInteger(1, 2147483647))
    v5[1] = bit32.bor(v41, 0)
    local v42 = v5[2]
    local v43 = v5[3]
    v5[3] = v42
    v5[2] = v43
    local v44 = v5[3]
    local v45 = v5[1]
    v5[1] = v44
    v5[3] = v45
    local v46 = getmetatable(v37)
    setmetatable(v39, v46)
    return v39
end

local function f5(v47, v48)
    local v49 = { v = v5[4] }
    local v50 = v49.v + (v47.v + v48.v)
    v49.v = bit32.bor(v50, 0)
    local v51 = v5[1] + (v49.v + v6:NextInteger(1, 2147483647))
    v5[1] = bit32.bor(v51, 0)
    local v52 = v5[4]
    local v53 = v5[1]
    v5[1] = v52
    v5[4] = v53
    local v54 = v5[4]
    local v55 = v5[1]
    v5[1] = v54
    v5[4] = v55
    local v56 = getmetatable(v47)
    setmetatable(v49, v56)
    return v49
end

local function f6(v57, v58)
    local v59 = { v = v5[4] }
    local v60 = v59.v - (v57.v + v58.v)
    v59.v = bit32.bor(v60, 0)
    local v61 = v5[1] - (v59.v + v6:NextInteger(1, 2147483647))
    v5[1] = bit32.bor(v61, 0)
    local v62 = v5[1]
    local v63 = v5[1]
    v5[1] = v62
    v5[1] = v63
    local v64 = v5[2]
    local v65 = v5[1]
    v5[1] = v64
    v5[2] = v65
    local v66 = getmetatable(v57)
    setmetatable(v59, v66)
    return v59
end

local function f7(v67, v68)
    local v69 = { v = v5[2] }
    local v70 = v69.v + (v67.v + v68.v)
    v69.v = bit32.bor(v70, 0)
    local v71 = v5[1] + (v69.v + v6:NextInteger(1, 2147483647))
    v5[1] = bit32.bor(v71, 0)
    local v72 = v5[4]
    local v73 = v5[2]
    v5[2] = v72
    v5[4] = v73
    local v74 = v5[3]
    local v75 = v5[1]
    v5[1] = v74
    v5[3] = v75
    local v76 = getmetatable(v67)
    setmetatable(v69, v76)
    return v69
end

local function f8(v77, v78)
    local v79 = { v = v5[3] }
    local v80 = v79.v - (v77.v - v78.v)
    v79.v = bit32.bor(v80, 0)
    local v81 = v5[1] + (v79.v - v6:NextInteger(1, 2147483647))
    v5[1] = bit32.bor(v81, 0)
    local v82 = v5[4]
    local v83 = v5[2]
    v5[2] = v82
    v5[4] = v83
    local v84 = v5[1]
    local v85 = v5[1]
    v5[1] = v84
    v5[1] = v85
    local v86 = getmetatable(v77)
    setmetatable(v79, v86)
    return v79
end

local v87 = {}

v87[1] = {
    __add = f2,
    __sub = f3
}
v87[2] = {
    __add = f4,
    __sub = f4
}

local v88 = {
    __add = f8,
    __sub = f1
}

v87[3] = v88

local v89 = {
    __add = f3,
    __sub = f3
}

v87[4] = v89

local v90 = v87[3]
setmetatable(v3, v90)
local v91 = v87[1]
setmetatable(v4, v91)
local v92 = {
    function(v93, v94)
        local v95 = { v = v5[2] }
        setmetatable(v95, v88)
        local v96 = v5[2] + v5[3]
        v5[2] = bit32.bor(v96, 0)
        local v97 = v95 + (v93 - v94)
        local v98 = f1(v97, v93)
        v5[1] -= v98.v + v6:NextInteger(1, 2147483647)
        return v98
    end,
    function(v99, v100)
        local v101 = { v = v5[3] }
        setmetatable(v101, v88)
        local v102 = v5[3] + v5[4]
        v5[3] = bit32.bor(v102, 0)
        local v103 = v101 - (v99 + v100)
        local v104 = f7(v103, v99)
        v5[1] -= v104.v + v6:NextInteger(1, 2147483647)
        return v104
    end,
    function(v105, v106)
        local v107 = { v = v5[3] }
        setmetatable(v107, v89)
        local v108 = v5[1] + v5[1]
        v5[1] = bit32.bor(v108, 0)
        local v109 = v107 + (v105 - v106)
        local v110 = f3(v109, v105)
        v5[1] += v110.v + v6:NextInteger(1, 2147483647)
        return v110
    end,
    function(v111, v112)
        local v113 = { v = v5[4] }
        setmetatable(v113, v88)
        local v114 = v5[3] + v5[2]
        v5[3] = bit32.bor(v114, 0)
        local v115 = v113 - (v111 - v112)
        local v116 = f1(v115, v111)
        v5[1] += v116.v - v6:NextInteger(1, 2147483647)
        return v116
    end
}
local v117 = #v92
for v118 = v117, 2, -1 do
    local v119 = v6:NextInteger(1, v118)
    local v120 = v92[v119]
    local v121 = v92[v118]
    v92[v118] = v120
    v92[v119] = v121
end
local v122 = v3 + v4
local v123 = v92[1](v122, v3)
local v124 = v92[2](v123, v4)
local v125 = v92[3](v124, v3)
local v126 = v92[4](v125, v4).v + v5[1]
local v127 = bit32.bor(v126, 0) + v5[2]
local v128 = bit32.bor(v127, 0) + v5[3]
local v129 = bit32.bor(v128, 0) + v5[4]
local v130 = bit32.bor(v129, 0)
local v131 = v130
local JobId = game.JobId
local v132 = #JobId
for v133 = 1, v132 do
    v130 += JobId:byte(v133)
    v131 = v130
end
local v134
xpcall(function()
    local function v135(...)
        return (tostring(...))
    end

    v134 = v135(UserSettings())
end, function()
    v134 = "nope"
end)
local v136 = #v134
for v137 = 1, v136 do
    v130 += v134:byte(v137)
    v131 = v130
end
local v138
xpcall(function(...)
    local function v139(...)
        return (tostring(...))
    end

    v138 = v139(UserSettings()) .. ...
end, function()
    v138 = "nope"
end, "yep")
local v140 = #v138
for v141 = 1, v140 do
    v130 += v138:byte(v141)
    v131 = v130
end
xpcall(function()
    os.exit()
end, function()
    v131 += 9001
end)
xpcall(function()
    v131 = game:GetService("RunService"):IsStudio() and v131 + 256 or v131 + 1024
end, function()
    v131 += 512
end)
xpcall(function()
    local v142 = newproxy(true)

    getmetatable(v142).__namecall = function(v143, v144)
        return 42 + v144
    end

    v131 += v142:Foo(10)
end, function()
    v131 += 2000
end)
return v131
