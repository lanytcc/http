add_rules("mode.debug", "mode.release")

add_requires("libcurl", "libevent")

includes("../quickjs/xmake.lua")

target("http")
    set_kind("shared")
    add_files("./*.c")
    add_deps("quickjs")
    set_languages("c11")
    add_packages("libcurl", "libevent")
    add_options("bignum", "atomics", "platform", "jsx", "smallest")