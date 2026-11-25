-- set minimum xmake version
set_xmakever("3.0.0")

-- include CommonLibF4 (git submodule)
includes("lib/commonlibf4")

-- set project
set_project("BloodFX")
set_version("0.1.0")
set_license("GPL-3.0")

-- set defaults
set_languages("c++23")
set_warnings("allextra")

-- add rules
add_rules("mode.debug", "mode.releasedbg")

-- targets
target("BloodFX", function()
    add_rules("commonlibf4.plugin", {
        name = "BloodFX",
        author = "dpillari",
        description = "GPU blood spray and pooling for any DX11 card"
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")

    add_syslinks("d3d11", "dxgi", "d3dcompiler", "user32", "kernel32")
end)
