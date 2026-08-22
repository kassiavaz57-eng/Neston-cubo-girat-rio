cmake_minimum_required(VERSION 3.13)

project(cubo_ps1_3d C ASM)

# Adiciona o executável informando que ele é STATIC e o arquivo principal
psn00bsdk_add_executable(cubo_ps1_3d STATIC main.c)

# Comando para gerar a ISO usando o nosso iso.xml
psn00bsdk_add_cd_image(iso cubo_ps1_3d iso.xml)
