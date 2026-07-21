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
* File Name        : websocket_utility.c
*
* Description      : WebSocket command console utility for the tester
*                    application. Provides a unified "ws" command with
*                    -c (client) and -s (server) modes.
*
*                    The architecture only supports one client and one server
*                    simultaneously.
*
* Unified command syntax
* ----------------------
*  Client mode (-c / --client):
*    ws -c connect <host> -p <port> [tls]
*    ws -c send -t <text|binary|ping> -m <message>
*    ws -c disconnect
*
*  Server mode (-s / --server):
*    ws -s start -p <port> [tls]
*    ws -s send -cid <client_id> -t <text|binary> -m <message>
*    ws -s stop
*
*******************************************************************************/
#ifndef DISABLE_COMMAND_CONSOLE_WEBSOCKET

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "websocket_utility.h"
#include "command_console.h"
#include "mtb_websocket.h"
#include "cy_wcm.h"
#include "websocket_certificates.h"

/*******************************************************************************
* Macros
*******************************************************************************/
#define WS_INFO(x)          printf x
#define WS_ERROR(x)         printf x

/** Maximum simultaneous clients tracked in the local registry. */
#define WS_MAX_CLIENTS      (MTB_WEBSOCKET_MAX_CLIENTS_PER_SERVER)

/** Maximum payload bytes accepted for a binary send command. */
#define WS_BINARY_BUF_SIZE  (256U)

/*******************************************************************************
* Type Definitions
*******************************************************************************/

/** One entry in the local client registry maintained by the server callback. */
typedef struct
{
    mtb_websocket_t handle;      /**< Handle returned by the library.          */
    uint8_t         client_id;   /**< Library-assigned client slot index.       */
    bool            valid;       /**< true when the slot is occupied.           */
} ws_client_entry_t;

/*******************************************************************************
* Static Variables
*******************************************************************************/
static bool              g_ws_initialized      = false;
static mtb_websocket_t   g_ws_client_handle    = NULL;
static mtb_websocket_t   g_ws_stale_handle     = NULL;
static mtb_websocket_t   g_ws_server_handle    = NULL;

static ws_client_entry_t g_ws_clients[WS_MAX_CLIENTS];
static uint8_t           g_ws_client_count = 0;

/*******************************************************************************
* Forward Declarations
*******************************************************************************/
static int cmd_ws(int argc, char *argv[], tlv_buffer_t **data);

/*******************************************************************************
* Command Table - single unified "ws" command
*******************************************************************************/
static const cy_command_console_cmd_t ws_command_table[] =
{
    { (char *)"ws",
      cmd_ws, 0, NULL, NULL,
      (char *)"-c|-s <action> [options]",
      (char *)"WebSocket utility. Use -c for client, -s for server." },

    CMD_TABLE_END
};

/*******************************************************************************
* Helper Functions
*******************************************************************************/

/**
 * Decode an even-length hex string into raw bytes.
 */
static int hex_str_to_bytes(const char *hex, uint8_t *out, size_t *out_len)
{
    size_t hex_len = strlen(hex);

    if (hex_len == 0 || (hex_len % 2) != 0)
    {
        return -1;
    }

    *out_len = hex_len / 2;
    if (*out_len > WS_BINARY_BUF_SIZE)
    {
        return -1;
    }

    for (size_t i = 0; i < *out_len; i++)
    {
        char byte_str[3] = { hex[2 * i], hex[2 * i + 1], '\0' };
        char *endptr     = NULL;
        long  val        = strtol(byte_str, &endptr, 16);

        if (*endptr != '\0' || val < 0 || val > 255)
        {
            return -1;
        }
        out[i] = (uint8_t)val;
    }

    return 0;
}

/**
 * Find a named argument value in argv.
 * Returns the value string following the flag, or NULL if not found.
 */
static const char *find_arg(int argc, char *argv[], const char *short_flag, const char *long_flag)
{
    for (int i = 0; i < argc - 1; i++)
    {
        if ((short_flag && strcmp(argv[i], short_flag) == 0) ||
            (long_flag && strcmp(argv[i], long_flag) == 0))
        {
            return argv[i + 1];
        }
    }
    return NULL;
}

/**
 * Check if a flag exists in argv (no value expected).
 */
static bool has_flag(int argc, char *argv[], const char *flag)
{
    for (int i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], flag) == 0)
        {
            return true;
        }
    }
    return false;
}

/**
 * Ensure the WebSocket library is initialized exactly once.
 */
static cy_rslt_t ws_ensure_initialized(void)
{
    if (g_ws_initialized)
    {
        return CY_RSLT_SUCCESS;
    }

    cy_rslt_t result = mtb_websocket_init();
    if (result != CY_RSLT_SUCCESS)
    {
        WS_ERROR(("[WS] mtb_websocket_init failed: 0x%lX\n", (unsigned long)result));
        return result;
    }

    g_ws_initialized = true;
    WS_INFO(("[WS] WebSocket library initialized.\n"));
    return CY_RSLT_SUCCESS;
}

/**
 * Look up a client handle by client_id.
 */
static mtb_websocket_t find_client_handle(uint8_t client_id)
{
    for (uint8_t i = 0; i < WS_MAX_CLIENTS; i++)
    {
        if (g_ws_clients[i].valid && g_ws_clients[i].client_id == client_id)
        {
            return g_ws_clients[i].handle;
        }
    }
    return NULL;
}

/*******************************************************************************
* Event Callback
*******************************************************************************/
static cy_rslt_t ws_event_callback(mtb_websocket_t websocket_handle,
                                   const mtb_websocket_event_t *event,
                                   void *user_data)
{
    (void)websocket_handle;
    (void)user_data;

    if (event == NULL)
    {
        return CY_RSLT_SUCCESS;
    }

    switch (event->type)
    {
        case MTB_WEBSOCKET_EVENT_CONNECTED:
            WS_INFO(("[WS] EVENT: Connected to server.\n"));
            break;

        case MTB_WEBSOCKET_EVENT_CLOSE:
            WS_INFO(("[WS] EVENT: Connection closed (code=%d).\n",
                     (int)event->close.code));
            if (websocket_handle == g_ws_client_handle)
            {
                /* Stash the handle for deferred cleanup. */
                g_ws_stale_handle  = g_ws_client_handle;
                g_ws_client_handle = NULL;
                WS_INFO(("[WS] Client handle marked for cleanup after remote close.\n"));
            }
            break;

        case MTB_WEBSOCKET_EVENT_MESSAGE:
        {
            const char *type_str = "UNKNOWN";
            if (event->message.msg_type == MTB_WEBSOCKET_MSG_TEXT)
                type_str = "TEXT";
            else if (event->message.msg_type == MTB_WEBSOCKET_MSG_BINARY)
                type_str = "BINARY";

            if (event->message.msg_type == MTB_WEBSOCKET_MSG_TEXT)
            {
                WS_INFO(("[WS] EVENT: Message [%s] (%u bytes): %.*s\n",
                         type_str,
                         (unsigned)event->message.length,
                         (int)event->message.length,
                         (const char *)event->message.data));
            }
            else
            {
                WS_INFO(("[WS] EVENT: Message [%s] (%u bytes)\n",
                         type_str, (unsigned)event->message.length));
            }
            break;
        }

        case MTB_WEBSOCKET_EVENT_PING:
            WS_INFO(("[WS] EVENT: Ping received.\n"));
            break;

        case MTB_WEBSOCKET_EVENT_PONG:
            WS_INFO(("[WS] EVENT: Pong received.\n"));
            break;

        case MTB_WEBSOCKET_EVENT_ERROR:
            WS_ERROR(("[WS] EVENT: Error (code=0x%lX).\n",
                      (unsigned long)event->error.error_code));
            break;

        case MTB_WEBSOCKET_EVENT_SERVER_STARTED:
            WS_INFO(("[WS] EVENT: Server started.\n"));
            break;

        case MTB_WEBSOCKET_EVENT_CLIENT_ACCEPTED:
        {
            uint8_t cid = event->client_connection.client_id;
            for (uint8_t i = 0; i < WS_MAX_CLIENTS; i++)
            {
                if (!g_ws_clients[i].valid)
                {
                    g_ws_clients[i].handle    = event->client_connection.client_handle;
                    g_ws_clients[i].client_id = cid;
                    g_ws_clients[i].valid     = true;
                    g_ws_client_count++;
                    break;
                }
            }
            WS_INFO(("[WS] EVENT: Client accepted (client_id=%d).\n", (int)cid));
            break;
        }

        case MTB_WEBSOCKET_EVENT_CLIENT_DISCONNECTED:
        {
            uint8_t cid = event->client_connection.client_id;
            for (uint8_t i = 0; i < WS_MAX_CLIENTS; i++)
            {
                if (g_ws_clients[i].valid && g_ws_clients[i].client_id == cid)
                {
                    g_ws_clients[i].valid = false;
                    g_ws_clients[i].handle = NULL;
                    if (g_ws_client_count > 0)
                        g_ws_client_count--;
                    break;
                }
            }
            WS_INFO(("[WS] EVENT: Client disconnected (client_id=%d).\n", (int)cid));
            break;
        }

        case MTB_WEBSOCKET_EVENT_SERVER_STOPPED:
            WS_INFO(("[WS] EVENT: Server stopped.\n"));
            break;

        case MTB_WEBSOCKET_EVENT_STATE_CHANGE:
            break;

        default:
            WS_INFO(("[WS] EVENT: Unknown event type %d.\n", (int)event->type));
            break;
    }

    return CY_RSLT_SUCCESS;
}

/*******************************************************************************
* Client Mode Handlers
*******************************************************************************/

/**
 * ws -c connect <host> -p <port> [tls]
 */
static int ws_client_connect(int argc, char *argv[])
{
    cy_rslt_t result;

    /* Find the host (positional: first arg after "connect") */
    const char *host = NULL;
    for (int i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], "connect") == 0 && (i + 1) < argc)
        {
            host = argv[i + 1];
            break;
        }
    }

    if (host == NULL)
    {
        WS_ERROR(("[WS] Usage: ws -c connect <host> -p <port> [tls]\n"));
        return ERR_INSUFFICENT_ARGS;
    }

    /* Find port */
    const char *port_str = find_arg(argc, argv, "-p", "--port");
    if (port_str == NULL)
    {
        WS_ERROR(("[WS] Usage: ws -c connect <host> -p <port> [tls]\n"));
        return ERR_INSUFFICENT_ARGS;
    }

    uint16_t port = (uint16_t)atoi(port_str);
    bool use_tls = has_flag(argc, argv, "tls");

    /* Auto-init library */
    result = ws_ensure_initialized();
    if (result != CY_RSLT_SUCCESS)
    {
        return ERR_UNKNOWN;
    }

    /* Clean up any stale handle left from a remote disconnect */
    if (g_ws_stale_handle != NULL)
    {
        mtb_websocket_delete(g_ws_stale_handle);
        g_ws_stale_handle = NULL;
    }

    /* Create client handle if not already created */
    if (g_ws_client_handle != NULL)
    {
        WS_ERROR(("[WS] Client already connected. Disconnect first.\n"));
        return ERR_UNKNOWN;
    }

    mtb_websocket_ssl_credentials_t tls_cfg;
    mtb_websocket_ssl_credentials_t *p_tls = NULL;

    if (use_tls)
    {
        memset(&tls_cfg, 0, sizeof(tls_cfg));
        tls_cfg.root_ca             = WS_ROOT_CA_CERTIFICATE;
        tls_cfg.root_ca_size        = sizeof(WS_ROOT_CA_CERTIFICATE);
        tls_cfg.root_ca_verify_mode = MTB_WEBSOCKET_ROOTCA_VERIFY_REQUIRED;
        tls_cfg.root_ca_location    = MTB_WEBSOCKET_CERT_KEY_LOCATION_RAM;
        tls_cfg.certificate         = WS_CLIENT_CERTIFICATE;
        tls_cfg.certificate_size    = sizeof(WS_CLIENT_CERTIFICATE);
        tls_cfg.private_key         = WS_CLIENT_PRIVATE_KEY;
        tls_cfg.private_key_size    = sizeof(WS_CLIENT_PRIVATE_KEY);
        tls_cfg.cert_key_location   = MTB_WEBSOCKET_CERT_KEY_LOCATION_RAM;
        p_tls = &tls_cfg;
        WS_INFO(("[WS] Creating TLS client (WSS)...\n"));
    }

    result = mtb_websocket_create_client(p_tls, &g_ws_client_handle);
    if (result != CY_RSLT_SUCCESS)
    {
        WS_ERROR(("[WS] mtb_websocket_create_client failed: 0x%lX\n",
                  (unsigned long)result));
        g_ws_client_handle = NULL;
        return ERR_UNKNOWN;
    }

    result = mtb_websocket_register_event_callback(g_ws_client_handle,
                                                   ws_event_callback, NULL);
    if (result != CY_RSLT_SUCCESS)
    {
        WS_ERROR(("[WS] Register callback failed: 0x%lX\n", (unsigned long)result));
    }

    /* Connect */
    mtb_websocket_server_info_t info;
    memset(&info, 0, sizeof(info));
    info.hostname     = host;
    info.hostname_len = (uint16_t)strlen(host);
    info.port         = port;

    WS_INFO(("[WS] Connecting to %s:%d%s...\n", host, (int)port,
             use_tls ? " (TLS)" : ""));

    result = mtb_websocket_connect(g_ws_client_handle, &info);
    if (result != CY_RSLT_SUCCESS)
    {
        WS_ERROR(("[WS] mtb_websocket_connect failed: 0x%lX\n",
                  (unsigned long)result));
        mtb_websocket_delete(g_ws_client_handle);
        g_ws_client_handle = NULL;
        return ERR_UNKNOWN;
    }

    WS_INFO(("[WS] Connected to %s:%d.\n", host, (int)port));
    return ERR_CMD_OK;
}

/**
 * ws -c send -t <text|binary|ping> -m <message>
 */
static int ws_client_send(int argc, char *argv[])
{
    if (g_ws_client_handle == NULL)
    {
        WS_ERROR(("[WS] No client connection. Use 'ws -c connect' first.\n"));
        return ERR_UNKNOWN;
    }

    const char *type_str = find_arg(argc, argv, "-t", "--type");
    if (type_str == NULL)
    {
        WS_ERROR(("[WS] Usage: ws -c send -t <text|binary|ping> -m <message>\n"));
        return ERR_INSUFFICENT_ARGS;
    }

    const char *msg = find_arg(argc, argv, "-m", "--message");
    cy_rslt_t result;

    if (strcmp(type_str, "text") == 0)
    {
        if (msg == NULL)
        {
            WS_ERROR(("[WS] Usage: ws -c send -t text -m <message>\n"));
            return ERR_INSUFFICENT_ARGS;
        }
        size_t len = strlen(msg);

        result = mtb_websocket_send(g_ws_client_handle,
                                    MTB_WEBSOCKET_MSG_TEXT,
                                    msg, len);
        if (result != CY_RSLT_SUCCESS)
        {
            WS_ERROR(("[WS] Send failed: 0x%lX\n", (unsigned long)result));
            return ERR_UNKNOWN;
        }
        WS_INFO(("[WS] Text sent (%u bytes): %s\n", (unsigned)len, msg));
    }
    else if (strcmp(type_str, "binary") == 0)
    {
        if (msg == NULL)
        {
            WS_ERROR(("[WS] Usage: ws -c send -t binary -m <hex_string>\n"));
            return ERR_INSUFFICENT_ARGS;
        }

        uint8_t bin_buf[WS_BINARY_BUF_SIZE];
        size_t  bin_len = 0;

        if (hex_str_to_bytes(msg, bin_buf, &bin_len) != 0)
        {
            WS_ERROR(("[WS] Invalid hex string.\n"));
            return ERR_BAD_ARG;
        }

        result = mtb_websocket_send(g_ws_client_handle,
                                    MTB_WEBSOCKET_MSG_BINARY,
                                    bin_buf, bin_len);
        if (result != CY_RSLT_SUCCESS)
        {
            WS_ERROR(("[WS] Send failed: 0x%lX\n", (unsigned long)result));
            return ERR_UNKNOWN;
        }
        WS_INFO(("[WS] Binary sent (%u bytes).\n", (unsigned)bin_len));
    }
    else if (strcmp(type_str, "ping") == 0)
    {
        const char *payload = msg;
        size_t      len     = (payload != NULL) ? strlen(payload) : 0;

        result = mtb_websocket_send(g_ws_client_handle,
                                    MTB_WEBSOCKET_MSG_PING,
                                    payload, len);
        if (result != CY_RSLT_SUCCESS)
        {
            WS_ERROR(("[WS] Ping send failed: 0x%lX\n", (unsigned long)result));
            return ERR_UNKNOWN;
        }
        WS_INFO(("[WS] Ping sent.\n"));
    }
    else
    {
        WS_ERROR(("[WS] Unknown type '%s'. Use 'text', 'binary', or 'ping'.\n",
                  type_str));
        return ERR_BAD_ARG;
    }

    return ERR_CMD_OK;
}

/**
 * ws -c disconnect
 */
static int ws_client_disconnect(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* Clean up any stale handle from a prior remote disconnect */
    if (g_ws_stale_handle != NULL)
    {
        mtb_websocket_delete(g_ws_stale_handle);
        g_ws_stale_handle = NULL;
    }

    if (g_ws_client_handle == NULL)
    {
        WS_ERROR(("[WS] No client connection.\n"));
        return ERR_UNKNOWN;
    }

    cy_rslt_t result = mtb_websocket_disconnect(g_ws_client_handle);
    if (result != CY_RSLT_SUCCESS)
    {
        WS_ERROR(("[WS] Disconnect failed: 0x%lX\n", (unsigned long)result));
    }

    result = mtb_websocket_delete(g_ws_client_handle);
    if (result != CY_RSLT_SUCCESS)
    {
        WS_ERROR(("[WS] Delete handle failed: 0x%lX\n", (unsigned long)result));
    }

    g_ws_client_handle = NULL;

    WS_INFO(("[WS] Client disconnected and handle released.\n"));
    return ERR_CMD_OK;
}

/*******************************************************************************
* Server Mode Handlers
*******************************************************************************/

/**
 * ws -s start -p <port> [tls]
 */
static int ws_server_start(int argc, char *argv[])
{
    cy_rslt_t result;

    const char *port_str = find_arg(argc, argv, "-p", "--port");
    if (port_str == NULL)
    {
        WS_ERROR(("[WS] Usage: ws -s start -p <port> [tls]\n"));
        return ERR_INSUFFICENT_ARGS;
    }

    uint16_t port = (uint16_t)atoi(port_str);
    bool use_tls = has_flag(argc, argv, "tls");

    /* Auto-init library */
    result = ws_ensure_initialized();
    if (result != CY_RSLT_SUCCESS)
    {
        return ERR_UNKNOWN;
    }

    if (g_ws_server_handle != NULL)
    {
        WS_ERROR(("[WS] Server already running. Stop first.\n"));
        return ERR_UNKNOWN;
    }

    mtb_websocket_ssl_credentials_t tls_cfg;
    mtb_websocket_ssl_credentials_t *p_tls = NULL;

    if (use_tls)
    {
        memset(&tls_cfg, 0, sizeof(tls_cfg));
        tls_cfg.certificate         = WS_SERVER_CERTIFICATE;
        tls_cfg.certificate_size    = sizeof(WS_SERVER_CERTIFICATE);
        tls_cfg.private_key         = WS_SERVER_PRIVATE_KEY;
        tls_cfg.private_key_size    = sizeof(WS_SERVER_PRIVATE_KEY);
        tls_cfg.cert_key_location   = MTB_WEBSOCKET_CERT_KEY_LOCATION_RAM;
        tls_cfg.root_ca             = WS_ROOT_CA_CERTIFICATE;
        tls_cfg.root_ca_size        = sizeof(WS_ROOT_CA_CERTIFICATE);
        tls_cfg.root_ca_verify_mode = MTB_WEBSOCKET_ROOTCA_VERIFY_NONE;
        tls_cfg.root_ca_location    = MTB_WEBSOCKET_CERT_KEY_LOCATION_RAM;
        p_tls = &tls_cfg;
        WS_INFO(("[WS] Creating TLS server (WSS)...\n"));
    }

    result = mtb_websocket_create_server(p_tls, &g_ws_server_handle);
    if (result != CY_RSLT_SUCCESS)
    {
        WS_ERROR(("[WS] mtb_websocket_create_server failed: 0x%lX\n",
                  (unsigned long)result));
        g_ws_server_handle = NULL;
        return ERR_UNKNOWN;
    }

    result = mtb_websocket_register_event_callback(g_ws_server_handle,
                                                   ws_event_callback, NULL);
    if (result != CY_RSLT_SUCCESS)
    {
        WS_ERROR(("[WS] Register callback failed: 0x%lX\n", (unsigned long)result));
    }

    g_ws_client_count = 0;
    memset(g_ws_clients, 0, sizeof(g_ws_clients));

    /* Bind and start */
    mtb_websocket_sockaddr_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.port               = port;
    addr.ip_address.version = CY_SOCKET_IP_VER_V4;
    addr.ip_address.ip.v4   = 0u;  /* INADDR_ANY */

    WS_INFO(("[WS] Starting server on port %d%s...\n", (int)port,
             use_tls ? " (TLS)" : ""));

    result = mtb_websocket_server_start(g_ws_server_handle,
                                        &addr, sizeof(addr),
                                        (uint8_t)WS_MAX_CLIENTS);
    if (result != CY_RSLT_SUCCESS)
    {
        WS_ERROR(("[WS] mtb_websocket_server_start failed: 0x%lX\n",
                  (unsigned long)result));
        mtb_websocket_delete(g_ws_server_handle);
        g_ws_server_handle = NULL;
        return ERR_UNKNOWN;
    }

    /* Print server URL */
    cy_wcm_ip_address_t ip_addr;
    memset(&ip_addr, 0, sizeof(ip_addr));
    if (cy_wcm_get_ip_addr(CY_WCM_INTERFACE_TYPE_STA, &ip_addr) == CY_RSLT_SUCCESS)
    {
        uint32_t ip = ip_addr.ip.v4;
        const char *scheme = use_tls ? "wss" : "ws";
        WS_INFO(("\n============================================\n"));
        WS_INFO((" WebSocket Server started!\n"));
        WS_INFO((" URL : %s://%lu.%lu.%lu.%lu:%d\n",
                 scheme,
                 (unsigned long)(ip & 0xFF),
                 (unsigned long)((ip >> 8) & 0xFF),
                 (unsigned long)((ip >> 16) & 0xFF),
                 (unsigned long)((ip >> 24) & 0xFF),
                 (int)port));
        WS_INFO(("============================================\n\n"));
    }
    else
    {
        WS_INFO(("[WS] Server started on port %d.\n", (int)port));
    }

    return ERR_CMD_OK;
}

/**
 * ws -s send -cid <client_id> -t <text|binary> -m <message>
 */
static int ws_server_send(int argc, char *argv[])
{
    if (g_ws_server_handle == NULL)
    {
        WS_ERROR(("[WS] No server running. Use 'ws -s start' first.\n"));
        return ERR_UNKNOWN;
    }

    const char *cid_str = find_arg(argc, argv, "-cid", "--clientid");
    if (cid_str == NULL)
    {
        WS_ERROR(("[WS] Usage: ws -s send -cid <client_id> -t <text|binary> -m <message>\n"));
        return ERR_INSUFFICENT_ARGS;
    }

    const char *type_str = find_arg(argc, argv, "-t", "--type");
    if (type_str == NULL)
    {
        WS_ERROR(("[WS] Usage: ws -s send -cid <client_id> -t <text|binary> -m <message>\n"));
        return ERR_INSUFFICENT_ARGS;
    }

    const char *msg = find_arg(argc, argv, "-m", "--message");
    if (msg == NULL)
    {
        WS_ERROR(("[WS] Usage: ws -s send -cid <client_id> -t <text|binary> -m <message>\n"));
        return ERR_INSUFFICENT_ARGS;
    }

    uint8_t client_id = (uint8_t)atoi(cid_str);
    mtb_websocket_t client_handle = find_client_handle(client_id);
    if (client_handle == NULL)
    {
        WS_ERROR(("[WS] client_id=%d not found.\n", (int)client_id));
        return ERR_BAD_ARG;
    }

    cy_rslt_t result;

    if (strcmp(type_str, "text") == 0)
    {
        size_t len = strlen(msg);
        result = mtb_websocket_send_to_client(client_handle,
                                              MTB_WEBSOCKET_MSG_TEXT,
                                              msg, len);
        if (result != CY_RSLT_SUCCESS)
        {
            WS_ERROR(("[WS] Send to client failed: 0x%lX\n", (unsigned long)result));
            return ERR_UNKNOWN;
        }
        WS_INFO(("[WS] Text sent to client_id=%d (%u bytes): %s\n",
                 (int)client_id, (unsigned)len, msg));
    }
    else if (strcmp(type_str, "binary") == 0)
    {
        uint8_t bin_buf[WS_BINARY_BUF_SIZE];
        size_t  bin_len = 0;

        if (hex_str_to_bytes(msg, bin_buf, &bin_len) != 0)
        {
            WS_ERROR(("[WS] Invalid hex string.\n"));
            return ERR_BAD_ARG;
        }

        result = mtb_websocket_send_to_client(client_handle,
                                              MTB_WEBSOCKET_MSG_BINARY,
                                              bin_buf, bin_len);
        if (result != CY_RSLT_SUCCESS)
        {
            WS_ERROR(("[WS] Send to client failed: 0x%lX\n", (unsigned long)result));
            return ERR_UNKNOWN;
        }
        WS_INFO(("[WS] Binary sent to client_id=%d (%u bytes).\n",
                 (int)client_id, (unsigned)bin_len));
    }
    else
    {
        WS_ERROR(("[WS] Unknown type '%s'. Use 'text' or 'binary'.\n", type_str));
        return ERR_BAD_ARG;
    }

    return ERR_CMD_OK;
}

/**
 * ws -s stop
 */
static int ws_server_stop(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (g_ws_server_handle == NULL)
    {
        WS_ERROR(("[WS] No server running.\n"));
        return ERR_UNKNOWN;
    }

    cy_rslt_t result = mtb_websocket_server_stop(g_ws_server_handle);
    if (result != CY_RSLT_SUCCESS)
    {
        WS_ERROR(("[WS] Server stop failed: 0x%lX\n", (unsigned long)result));
    }

    result = mtb_websocket_delete(g_ws_server_handle);
    if (result != CY_RSLT_SUCCESS)
    {
        WS_ERROR(("[WS] Delete server handle failed: 0x%lX\n", (unsigned long)result));
    }

    g_ws_server_handle = NULL;
    g_ws_client_count  = 0;
    memset(g_ws_clients, 0, sizeof(g_ws_clients));
    WS_INFO(("[WS] Server stopped and handle released.\n"));
    return ERR_CMD_OK;
}

/*******************************************************************************
* Unified ws Command Dispatcher
*******************************************************************************/

static void ws_print_usage(void)
{
    WS_INFO(("\nWebSocket Utility Commands:\n"));
    WS_INFO(("  Client mode:\n"));
    WS_INFO(("    ws -c connect <host> -p <port> [tls]\n"));
    WS_INFO(("    ws -c send -t <text|binary|ping> -m <message>\n"));
    WS_INFO(("    ws -c disconnect\n"));
    WS_INFO(("  Server mode:\n"));
    WS_INFO(("    ws -s start -p <port> [tls]\n"));
    WS_INFO(("    ws -s send -cid <client_id> -t <text|binary> -m <message>\n"));
    WS_INFO(("    ws -s stop\n\n"));
}

static int cmd_ws(int argc, char *argv[], tlv_buffer_t **data)
{
    (void)data;

    if (argc < 3)
    {
        ws_print_usage();
        return ERR_INSUFFICENT_ARGS;
    }

    const char *mode   = argv[1];  /* -c or -s */
    const char *action = argv[2];  /* connect, send, disconnect, start, stop */

    /* Client mode */
    if (strcmp(mode, "-c") == 0 || strcmp(mode, "--client") == 0)
    {
        if (strcmp(action, "connect") == 0)
        {
            return ws_client_connect(argc, argv);
        }
        else if (strcmp(action, "send") == 0)
        {
            return ws_client_send(argc, argv);
        }
        else if (strcmp(action, "disconnect") == 0)
        {
            return ws_client_disconnect(argc, argv);
        }
        else
        {
            WS_ERROR(("[WS] Unknown client action '%s'.\n", action));
            ws_print_usage();
            return ERR_BAD_ARG;
        }
    }
    /* Server mode */
    else if (strcmp(mode, "-s") == 0 || strcmp(mode, "--server") == 0)
    {
        if (strcmp(action, "start") == 0)
        {
            return ws_server_start(argc, argv);
        }
        else if (strcmp(action, "send") == 0)
        {
            return ws_server_send(argc, argv);
        }
        else if (strcmp(action, "stop") == 0)
        {
            return ws_server_stop(argc, argv);
        }
        else
        {
            WS_ERROR(("[WS] Unknown server action '%s'.\n", action));
            ws_print_usage();
            return ERR_BAD_ARG;
        }
    }
    else
    {
        WS_ERROR(("[WS] Unknown mode '%s'. Use -c (client) or -s (server).\n", mode));
        ws_print_usage();
        return ERR_BAD_ARG;
    }
}

/*******************************************************************************
* Public API
*******************************************************************************/

cy_rslt_t websocket_utility_init(void)
{
    cy_command_console_add_table(ws_command_table);

    WS_INFO(("WebSocket utility commands registered.\n"
             "Type 'ws' for usage.\n"));

    return CY_RSLT_SUCCESS;
}

#endif /* DISABLE_COMMAND_CONSOLE_WEBSOCKET */

/* [] END OF FILE */
