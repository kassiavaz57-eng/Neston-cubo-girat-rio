cmake_minimum_required(VERSION 3.21)

project(cubo_ps1_3d LANGUAGES C ASM)

# Add homebrew executable target
psn00bsdk_add_executable(cubo_ps1_3d GPREL main.c)

# Link PSn00bSDK libraries
target_link_libraries(
    cubo_ps1_3d
    PUBLIC
    psxgpu
    psxgte
    psxetc
    psxspu
    psxapi
    c
)

# Build ISO Image
psn00bsdk_add_cd_image(
    iso
    cubo_ps1_3d
    iso.xml
    DEPENDS cubo_ps1_3d
)
