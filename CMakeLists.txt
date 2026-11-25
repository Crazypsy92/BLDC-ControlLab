cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME BLDC-ControlLab)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # ----- LL 驱动（TIM / I2C / SPI，上一轮已拉取，未进 CubeMX 生成列表） -----
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_tim.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_i2c.c
    Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_spi.c
    # ----- 用户外设初始化 -----
    Core/Src/tim.c
    Core/Src/i2c.c
    # ----- BSP（GC9A01 / W25QXX 双 SPI 总线，裸机无 RTOS） -----
    BSP/Src/delay.c
    BSP/Src/bsp_spi.c
    BSP/Src/spi_stm32_ll.c
    BSP/Src/gc9a01.c
    BSP/Src/gc9a01_port.c
    BSP/Src/w25qxx.c
    BSP/Src/w25qxx_port.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    BSP/Inc
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    CONFIG_DUAL_SPI=1
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
