import build


build.includes += ["$(S)/../std"]
build.cxxflags += [
    "-W",
    "-Wall",
    "-std=c++26",
    "-O2",
    "-g",
    "-fno-omit-frame-pointer",
    "-mno-omit-leaf-frame-pointer",
]


std = dependency(
    ldflags=["-L$(S)/../std/std", "-lstd"],
)
mnl = pkg_config("libmnl")


gofra = program(
    srcs=build.glob("$(S)/*.cpp"),
    deps=[std, mnl],
)


install(gofra)
