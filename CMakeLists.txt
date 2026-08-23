cmake_minimum_required(VERSION 3.13)

project(cubo_ps1_3d C ASM)

# Compila o jogo e gera:
# cubo_ps1_3d.elf
# cubo_ps1_3d.exe
# cubo_ps1_3d.map
psn00bsdk_add_executable(
    cubo_ps1_3d
    STATIC
    main.c
)

# Gera BIN + CUE depois que o executável estiver pronto
psn00bsdk_add_cd_image(
    iso
    cubo_ps1_3d
    iso.xml
    DEPENDS cubo_ps1_3d
)
