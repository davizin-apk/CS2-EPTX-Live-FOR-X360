# Bot Strike 360: o Bot Strike inteiro no Xbox 360, com o xsdk. Compile pelo build.sh (dentro do WSL).
cmake_minimum_required(VERSION 3.25)
project(botstrike C)

# O nucleo do cliente do EPTX (sessao, requisicoes assinadas, JSON), o mesmo da dash do EPTX.
set(EPTX_CORE "/mnt/d/EPTX Live/client/core" CACHE PATH "client/core do EPTX Live")

add_executable(botstrike
	src/main.c src/assets.c src/anim.c src/audio.c src/build.c src/collide.c src/fighter.c src/match.c src/pack.c
	src/player.c src/portal.c src/ui.c src/world.c src/net.c src/netgame.c src/gfx_xbox.c src/plat_xbox.c
	"${EPTX_CORE}/eptx_client.c" "${EPTX_CORE}/eptx_http.c" "${EPTX_CORE}/eptx_json.c" "${EPTX_CORE}/eptx_sign.c"
	"${EPTX_CORE}/eptx_sha256.c")
target_include_directories(botstrike PRIVATE "${EPTX_CORE}")
set_target_properties(botstrike PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(botstrike PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
target_include_directories(botstrike PRIVATE src)
target_link_libraries(botstrike m)
# Escreve botstrike_shaders.h.
xsdk_add_shaders(botstrike shaders/map.vert shaders/map.frag shaders/map_alpha.frag shaders/model.vert shaders/fx.vert
	shaders/fx.frag shaders/fx_alpha.frag)
# Title ID proprio ("BS36"), para nao misturar nada com outro titulo.
xsdk_add_xex(botstrike TITLE_ID 0x42533336 NAME "Bot Strike 360" VERSION 0.2.0.0)
