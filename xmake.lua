set_project("space_saving")
set_version("1.0.0")

add_rules("mode.debug", "mode.release")
set_defaultmode("release")

set_languages("c++26")
set_warnings("allextra", "pedantic")
add_cxxflags("-Wconversion", "-Wshadow")

-- header-only library
target("space_saving")
    set_kind("headeronly")
    add_includedirs("include", {public = true})
    add_headerfiles("include/(space_saving/**.hpp)")

target("test_space_saving")
    set_kind("binary")
    add_files("tests/test_space_saving.cpp")
    add_deps("space_saving")
    add_tests("default")

target("test_baselines")
    set_kind("binary")
    add_files("tests/test_baselines.cpp")
    add_deps("space_saving")
    add_tests("default")

target("test_exception_safety")
    set_kind("binary")
    add_files("tests/test_exception_safety.cpp")
    add_deps("space_saving")
    add_tests("default")

target("test_experiment")
    set_kind("binary")
    add_files("tests/test_experiment.cpp")
    add_includedirs("bench")
    add_deps("space_saving")
    add_tests("default")

target("paper_example")
    set_kind("binary")
    add_files("examples/paper_example.cpp")
    add_deps("space_saving")
    add_tests("default")

target("zipf_experiment")
    set_kind("binary")
    add_files("bench/zipf_experiment.cpp")
    add_deps("space_saving")
    add_tests("smoke", {runargs = {"--n=200000", "--alphabet=20000"}})
