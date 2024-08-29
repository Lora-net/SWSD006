# Copyright (c) 2021 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#
# This CMake file is picked by the Zephyr build system because it is defined
# as the module CMake entry point (see zephyr/module.yml).

if(CONFIG_RADIO_LR11XX)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    Set(script_name "scripts/check_for_patch.py")
    Set(file_to_search "../sidewalk/subsys/sal/sid_pal/src/sid_common.c")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${script_name} ${file_to_search}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE ret
    )
    if(ret EQUAL "0")  # if(ret AND NOT ret EQUAL 0
        message( STATUS "patch already applied")
    else()
        message( STATUS "patch needs to be applied?  ret=" ${ret})
        Set(script_name ${CMAKE_CURRENT_SOURCE_DIR}/scripts/patch_ng.py)
        Set(args "-p 1")
        Set(pfile ${CMAKE_CURRENT_SOURCE_DIR}/v270_sidewalk_lr11xx.diff)
        execute_process(
            COMMAND ${Python3_EXECUTABLE} ${script_name} ${args} ${pfile}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../sidewalk
            RESULT_VARIABLE ret
        )
        # success is ret 0
        if(NOT ret EQUAL "0")
            message(FATAL_ERROR "patch_ng failed, ret " ${ret})
        else()
            message(STATUS "OK patch_ng")
        endif()
    endif()

    zephyr_include_directories(include)
	if(CONFIG_SIDEWALK_GPIO_MAX LESS 10)
		message(FATAL_ERROR "CONFIG_SIDEWALK_GPIO_MAX must be at least 10")
	endif()
endif()

zephyr_include_directories(../sidewalk/tests/validation/timer/src) # get sid_clock_ifc.h

if(NOT DEFINED CONFIG_MCUBOOT)
    add_subdirectory(lib)
	#if(CONFIG_LORABASICSMODEM_V431)
	#	set(LBM_DIR lbm_lib_v431)
	#elseif(CONFIG_LORABASICSMODEM_V450)
	#	set(LBM_DIR lbm_lib_v450)
	#endif()
	if(CONFIG_LORABASICSMODEM_V431 OR CONFIG_LORABASICSMODEM_V450)
		#add_subdirectory(${LBM_DIR})
        add_subdirectory(lbm_v4_sidewalk)
	endif()
endif()
