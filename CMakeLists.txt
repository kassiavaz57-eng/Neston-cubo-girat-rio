cmake_minimum_required(VERSION 3.13)
include(psn00bsdk)

project(cubo_ps1_3d)

# Use esta função do SDK em vez do add_executable comum!
psn00bsdk_add_executable(cubo_ps1_3d main.c)

# E o comando para criar a ISO usando o iso.xml que a gente ajustou:
psn00bsdk_add_cd_image(iso cubo_ps1_3d iso.xml)
