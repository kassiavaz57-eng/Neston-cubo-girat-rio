cmake_minimum_required(VERSION 3.13)
project(cubo_ps1_3d C ASM)

# Cria o executável usando o padrão do PSn00bSDK
psn00bsdk_add_executable(cubo_ps1_3d STATIC main.c)

# Deixa o próprio SDK gerar a ISO corretamente
psn00bsdk_add_cd_image(iso cubo_ps1_3d iso.xml)
