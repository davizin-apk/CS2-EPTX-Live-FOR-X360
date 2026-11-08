# EPTX NXE Edition como titulo do xsdk: o nucleo de client/core, a rede em app.c e a
# tela em dash.cpp, que desenha as cenas do Freestyle Dash pelo xui.cpp com o Dear
# ImGui do imgui-xenon. Compila no WSL; veja client/xbox/build.sh.
cmake_minimum_required(VERSION 3.25)
project(eptx_nxe C CXX)

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../core)
set(IMGUI_DIR "$ENV{HOME}/imgui-xenon" CACHE PATH "clone do github.com/xenosworks/imgui-xenon")
if(NOT EXISTS "${IMGUI_DIR}/imgui.h")
	message(FATAL_ERROR "imgui-xenon nao esta em ${IMGUI_DIR}; clone ou passe -DIMGUI_DIR=...")
endif()

# O ImGui como upstream entrega; o shell padrao dele chama fork(), que o console nao tem.
add_library(imgui STATIC "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp" "${IMGUI_DIR}/imgui_tables.cpp"
	"${IMGUI_DIR}/imgui_widgets.cpp")
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS)

# As fontes vao dentro do .xex: dentro do pacote XBLA, ler de game: e possivel, mas
# um arquivo a menos para faltar e melhor.
set(FONT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/selawik)
set(FONTS_C ${CMAKE_CURRENT_BINARY_DIR}/fonts.c)
set(FONTS_CONTENT "/* Gerado pelo CMakeLists.txt a partir de third_party/selawik. */\n")
foreach(pair "font_regular:selawk.ttf" "font_semibold:selawksb.ttf" "font_light:selawkl.ttf")
	string(REPLACE ":" ";" pair "${pair}")
	list(GET pair 0 symbol)
	list(GET pair 1 file)
	file(READ "${FONT_DIR}/${file}" hex HEX)
	string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
	string(APPEND FONTS_CONTENT "const unsigned char ${symbol}[] = {${hex}};\n")
	string(APPEND FONTS_CONTENT "const unsigned int ${symbol}_size = sizeof(${symbol});\n")
endforeach()
file(CONFIGURE OUTPUT ${FONTS_C} CONTENT "${FONTS_CONTENT}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
	${FONT_DIR}/selawk.ttf ${FONT_DIR}/selawksb.ttf ${FONT_DIR}/selawkl.ttf)

add_executable(eptx_nxe
	dash.cpp
	xui.cpp
	sound.c
	app.c
	image.c
	${FONTS_C}
	"${IMGUI_DIR}/backends/imgui_impl_xsdk.cpp"
	${CORE}/eptx_sha256.c
	${CORE}/eptx_sign.c
	${CORE}/eptx_json.c
	${CORE}/eptx_http.c
	${CORE}/eptx_client.c
)
target_include_directories(eptx_nxe PRIVATE ${CORE} ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
target_link_libraries(eptx_nxe PRIVATE imgui)
set_target_properties(eptx_nxe PROPERTIES C_STANDARD 17 C_EXTENSIONS ON CXX_STANDARD 17)
target_compile_options(eptx_nxe PRIVATE -Wall -Wextra -Werror)
# 0x4550584E e "EPXN" em ASCII: outro Title ID que o do EPTX Live (0x45505458, "EPTX"),
# para os dois pacotes morarem em pastas diferentes de Hdd1:\Content e nao se sobrescreverem.
xsdk_add_xex(eptx_nxe TITLE_ID 0x4550584E NAME "EPTX NXE Edition" VERSION 0.1.0.0)
