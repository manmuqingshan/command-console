/*
 * (c) 2026, Infineon Technologies AG, or an affiliate of Infineon
 * Technologies AG. All rights reserved.
 * This software, associated documentation and materials ("Software") is
 * owned by Infineon Technologies AG or one of its affiliates ("Infineon")
 * and is protected by and subject to worldwide patent protection, worldwide
 * copyright laws, and international treaty provisions. Therefore, you may use
 * this Software only as provided in the license agreement accompanying the
 * software package from which you obtained this Software. If no license
 * agreement applies, then any use, reproduction, modification, translation, or
 * compilation of this Software is prohibited without the express written
 * permission of Infineon.
 *
 * Disclaimer: UNLESS OTHERWISE EXPRESSLY AGREED WITH INFINEON, THIS SOFTWARE
 * IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING, BUT NOT LIMITED TO, ALL WARRANTIES OF NON-INFRINGEMENT OF
 * THIRD-PARTY RIGHTS AND IMPLIED WARRANTIES SUCH AS WARRANTIES OF FITNESS FOR A
 * SPECIFIC USE/PURPOSE OR MERCHANTABILITY.
 * Infineon reserves the right to make changes to the Software without notice.
 * You are responsible for properly designing, programming, and testing the
 * functionality and safety of your intended application of the Software, as
 * well as complying with any legal requirements related to its use. Infineon
 * does not guarantee that the Software will be free from intrusion, data theft
 * or loss, or other breaches ("Security Breaches"), and Infineon shall have
 * no liability arising out of any Security Breaches. Unless otherwise
 * explicitly approved by Infineon, the Software may not be used in any
 * application where a failure of the Product or any consequences of the use
 * thereof can reasonably be expected to result in personal injury.
 */

/*******************************************************************************
* File Name        : websocket_utility.h
*
* Description      : WebSocket command console utility for the tester
*                    application.  Registers a set of console commands that
*                    exercise every public API in the mtb_websocket library so
*                    that both client and server functionality can be verified
*                    interactively from a serial terminal.
*
*******************************************************************************/

#pragma once

#include "cy_result_mw.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the WebSocket command console utility.
 *
 * Registers all ws_* commands with the command console framework.
 * Call this after cy_command_console_init() and before starting the FreeRTOS
 * scheduler (or from any task after the scheduler is running).
 *
 * The WebSocket library is auto-initialized on the first ws_create_client
 * or ws_create_server command.
 *
 * @return CY_RSLT_SUCCESS on success; error code otherwise.
 */
cy_rslt_t websocket_utility_init(void);

#ifdef __cplusplus
}
#endif

/* [] END OF FILE */
