/*
 * Copyright (c) 2020, Arm Limited and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <string.h>
#include "TELIT_ME910_CellularStack.h"
#include "CellularLog.h"
#include "netsocket/TLSSocket.h"

#include "platform/Callback.h"

#include "rtos/ThisThread.h"
#include <chrono>

using namespace::std::chrono;


using namespace mbed;
using namespace std::chrono_literals;

constexpr int sslctxID = 1;
constexpr auto socket_timeout = 1s;
uint8_t g_sslsring_en = 0;


TELIT_ME910_CellularStack::TELIT_ME910_CellularStack(ATHandler &atHandler, int cid, nsapi_ip_stack_t stack_type, AT_CellularDevice &device) :
    AT_CellularStack(atHandler, cid, stack_type, device)
    , _tls_sec_level(0)
#ifdef MBED_CONF_CELLULAR_OFFLOAD_DNS_QUERIES
    , _dns_callback(nullptr), _dns_version(NSAPI_UNSPEC)
#endif
{

#ifdef MBED_CONF_CELLULAR_OFFLOAD_DNS_QUERIES
    _at.set_urc_handler("#QDNS", mbed::callback(this, &TELIT_ME910_CellularStack::urc_qdns));
#endif

    _at.set_urc_handler("SRING:", mbed::callback(this, &TELIT_ME910_CellularStack::urc_sring));
    _at.set_urc_handler("SSLSRING:", mbed::callback(this, &TELIT_ME910_CellularStack::urc_sring));

    _at.set_send_delay(15);

    //_at.set_at_timeout(5s, true);

    /* Close all SSL sockets if open. This can happen for example if application processor
     * was reset but modem not. Old sockets are still up and running and it prevents
     * new SSL configurations and creating new sockets.
     */
//    for (int i = 1; i <= ME910_SOCKET_MAX; i++) {
//        char response[16];
//        _at.clear_error();
//        _at.set_delimiter(',');
//        tr_debug("Checking SSL socket %d...", i);
//        nsapi_error_t err = _at.at_cmd_discard("#SS", "=", "%d", i);
//
//        if(err) {
//            tr_err("Error when checking SSL socket status: %d", err);
//        } else {
//            /* Response format: #SS: <socket_id>,<status>*/
//            _at.resp_start("#SS: ");
//            _at.skip_param(1);
//            int32_t state = _at.read_int();
//            if(state) {
//                /* Close the socket */
//                tr_debug("Closing open SSL socket %d...", i);
//                _at.at_cmd_discard("#SSLH", "=", "%d%d", i, 0);
//            }
//        }
//        _at.set_default_delimiter();
//        _at.clear_error();
//    }
}

TELIT_ME910_CellularStack::~TELIT_ME910_CellularStack()
{
}

nsapi_error_t TELIT_ME910_CellularStack::socket_listen(nsapi_socket_t handle, int backlog)
{
    return NSAPI_ERROR_UNSUPPORTED;
}

nsapi_error_t TELIT_ME910_CellularStack::socket_accept(void *server, void **socket, SocketAddress *addr)
{
    return NSAPI_ERROR_UNSUPPORTED;
}

nsapi_error_t TELIT_ME910_CellularStack::socket_connect(nsapi_socket_t handle, const SocketAddress &address)
{
    CellularSocket *socket = (CellularSocket *)handle;
    nsapi_error_t err = NSAPI_ERROR_OK;

    if (!is_ipeasy_context_activated(_cid)) {
        // Retry up to 10 times
        for(int i = 0; i < 10; i++ ) {
            err = activate_ipeasy_context(_cid);
            if(err == NSAPI_ERROR_OK) {
                break;
            } else {
                rtos::ThisThread::sleep_for(100ms);
            }
        }
        // Hit some sort of error opening the socket
        if(err != NSAPI_ERROR_OK) {
            socket->id = -1;
            return NSAPI_ERROR_PARAMETER;
        }
    }

    err = NSAPI_ERROR_NO_CONNECTION;

    int request_connect_id = find_socket_index(socket);
    // assert here as its a programming error if the socket container doesn't contain
    // specified handle
    MBED_ASSERT(request_connect_id != -1);

    rtos::ThisThread::sleep_for(50ms);

    _at.lock();

    // Configure SRING URC
    _at.at_cmd_discard("#SCFGEXT", "=", "%d%d%d%d",
                       request_connect_id + 1,
                       1,   // SRING URC mode - data amount mode
                       0,   // Data view mode - text mode
                       0);  // TCP keepalive - deactivated
    if (_at.get_last_error() != NSAPI_ERROR_OK) {
        tr_error("Unable to configure socket %d", request_connect_id);
        _at.unlock();
        return NSAPI_ERROR_DEVICE_ERROR;
    }

    if (socket->proto == NSAPI_TCP) {
        if (socket->tls_socket) {
            if (_tls_sec_level == 0) {
                _at.unlock();
                return NSAPI_ERROR_AUTH_FAILURE;
            }

            _at.at_cmd_discard("#SSLD", "=", "%d%d%s%d", sslctxID,
                               address.get_port(),
                               address.get_ip_address(),
                               0); // Closure type (0)
            if (_at.get_last_error() != NSAPI_ERROR_OK) {
                // Hit some sort of error opening the socket
                socket->id = -1;
                _at.unlock();
                return NSAPI_ERROR_PARAMETER;
            }
        } else {
            // Retry up to 10 times
            for(int i = 0; i < 10; i++) {
                _at.at_cmd_discard("#SD", "=", "%d%d%d%s%d%d%d", request_connect_id + 1, 0, address.get_port(), address.get_ip_address(), 0,
                                   0, 1);
                err = _at.get_last_error();
                if (err == NSAPI_ERROR_OK) {
                    break;
                } else {
                    /* Sleep for a bit... maybe the socket instance isn't ready yet */
                    rtos::ThisThread::sleep_for(100ms);
                }
            }

            // Hit some sort of error opening the socket
            if(err != NSAPI_ERROR_OK) {
                socket->id = -1;
                _at.unlock();
                return NSAPI_ERROR_PARAMETER;
            }
        }
    }

    nsapi_error_t ret_val = _at.get_last_error();
    _at.unlock();

    if (ret_val == NSAPI_ERROR_OK) {
        socket->id = request_connect_id;
        socket->remoteAddress = address;
        socket->connected = true;
        return NSAPI_ERROR_OK;
    }

    return err;
}

void TELIT_ME910_CellularStack::urc_sring()
{
    _at.lock();
    const int sock_id = _at.read_int() - 1;
    const int data_bytes_remaining = _at.read_int();
    const nsapi_error_t err = _at.unlock_return_error();

    if (err != NSAPI_ERROR_OK) {
        return;
    }

    CellularSocket *sock = find_socket(sock_id);
    if (sock) {
        sock->pending_bytes = data_bytes_remaining;
        if (sock->_cb) {
            sock->_cb(sock->_data);
        }
    }
}

int TELIT_ME910_CellularStack::get_max_socket_count()
{
    return ME910_SOCKET_MAX;
}

bool TELIT_ME910_CellularStack::is_protocol_supported(nsapi_protocol_t protocol)
{
    return (protocol == NSAPI_UDP || protocol == NSAPI_TCP);
}

nsapi_error_t TELIT_ME910_CellularStack::socket_close_impl(int sock_id)
{
    _at.set_at_timeout(ME910_CLOSE_SOCKET_TIMEOUT);
    nsapi_error_t err;
    CellularSocket *socket = find_socket(sock_id);
    if (socket && socket->tls_socket) {
        err = _at.at_cmd_discard("#SSLH", "=", "%d%d", sslctxID, 0);
        if (err == NSAPI_ERROR_OK) {
            // Disable TLSSocket settings to prevent reuse on next socket without setting the values
            _tls_sec_level = 0;
            err = _at.at_cmd_discard("#SSLEN", "=", "%d%d", sslctxID, 0);
        }
    } else {
        err = _at.at_cmd_discard("#SH", "=", "%d", sock_id + 1);
    }
    _at.restore_at_timeout();

    return err;
}

bool TELIT_ME910_CellularStack::is_ipeasy_context_activated(int context_id)
{
    _at.lock();

    _at.cmd_start_stop("#SGACT?", "");
    _at.resp_start("#SGACT:");

    int current_context_id = -1;
    int current_stat = -1;

    for (int i = 0; i < ME910_CONTEXT_MAX; i++) {
        current_context_id = _at.read_int();
        current_stat = _at.read_int();

        if (current_context_id == context_id) {
            _at.resp_stop();
            _at.unlock();
            return current_stat == ME910_IPEASY_ACTIVATED_CONTEXT;
        }
    }

    _at.resp_stop();
    _at.unlock();
    return false;
}

nsapi_error_t TELIT_ME910_CellularStack::activate_ipeasy_context(int context_id)
{
    _at.lock();

    _at.at_cmd_discard("#SGACT", "=", "%d%d", context_id, ME910_IPEASY_ACTIVATED_CONTEXT);

    return _at.unlock_return_error();
}

nsapi_error_t TELIT_ME910_CellularStack::deactivate_ipeasy_context(int context_id)
{
    _at.lock();

    _at.at_cmd_discard("#SGACT", "=", "%d%d", context_id, ME910_IPEASY_DEACTIVATED_CONTEXT);

    return _at.unlock_return_error();
}

nsapi_error_t TELIT_ME910_CellularStack::create_socket_impl(CellularSocket *socket)
{
    int remote_port = 1;
    nsapi_error_t ret_val = NSAPI_ERROR_OK;

    if (!is_ipeasy_context_activated(_cid)) {
        tr_debug("IPEasy context not active for %d", _cid);
        activate_ipeasy_context(_cid);
    }

    int request_connect_id = find_socket_index(socket);
    // assert here as its a programming error if the socket container doesn't contain
    // specified handle
    MBED_ASSERT(request_connect_id != -1);

    // Configure SRING URC
    _at.at_cmd_discard("#SCFGEXT", "=", "%d%d%d%d",
                       request_connect_id + 1,
                       1,   // SRING URC mode - data amount mode
                       0,   // Data view mode - text mode
                       0);  // TCP keepalive - deactivated
    ret_val = _at.get_last_error();
    if (ret_val != NSAPI_ERROR_OK) {
        tr_warn("Unable to configure socket %d", request_connect_id);
        return ret_val;
    } else {
        socket->id = request_connect_id;
        return ret_val;
    }
}

nsapi_size_or_error_t TELIT_ME910_CellularStack::socket_sendto_impl(CellularSocket *socket, const SocketAddress &address,
                                                                    const void *data, nsapi_size_t size)
{
    tr_debug("TELIT_ME910_CellularStack::socket_sendto_impl()");

    int sent_len = 0;
    bool success = true;
    const char *buf = (const char *) data;
    nsapi_size_t blk = ME910_MAX_SEND_SIZE;
    nsapi_size_t count = size;
    int sent_len_before = 0;
    int sent_len_after = 0;
    Timer timer;

    timer.start();
    while ((count > 0) && success) {

        if (count < blk) {
            blk = count;
        }

        if (socket->tls_socket) {
            sent_len_after = blk;
        } else {
            // Get the sent count before sending
            _at.set_at_timeout(ME910_SEND_SOCKET_TIMEOUT);
            _at.cmd_start_stop("#SI", "=", "%d", socket->id + 1);
            _at.resp_start("#SI:");
            _at.skip_param();
            sent_len_before = _at.read_int();
            _at.resp_stop();
            _at.restore_at_timeout();
        }

        // Send
        if (socket->proto == NSAPI_UDP) {
            _at.cmd_start_stop("#SSENDUDPEXT", "=", "%d%d%s%d", socket->id + 1, blk,
                               address.get_ip_address(), address.get_port());
        } else {
            if (socket->tls_socket) {
                _at.cmd_start_stop("#SSLSENDEXT", "=", "%d%d", socket->id + 1, blk);
            } else {
                _at.cmd_start_stop("#SSENDEXT", "=", "%d%d", socket->id + 1, blk);
            }
        }

        _at.resp_start("> ", true);
        _at.write_bytes((uint8_t *)buf, blk);

        _at.cmd_start(CTRL_Z);
        _at.cmd_stop();
        _at.resp_start("\r\nOK", true);
        _at.resp_stop();

        if (_at.get_last_error() != NSAPI_ERROR_OK) {
            success = false;
            break;
        }

        if (!socket->tls_socket) {
            _at.set_at_timeout(ME910_SEND_SOCKET_TIMEOUT);
            _at.cmd_start_stop("#SI", "=", "%d", socket->id + 1);
            _at.resp_start("#SI:");
            _at.skip_param();
            sent_len_after = _at.read_int();
            _at.resp_stop();
            _at.restore_at_timeout();
        }

        sent_len = sent_len_after - sent_len_before;

        if ((sent_len >= (int) blk) &&
                (_at.get_last_error() == NSAPI_ERROR_OK)) {
        } else {
            success = false;
        }

        if (timer.elapsed_time() > (socket_timeout * 5)) {
            success = false;
        }

        buf += blk;
        count -= blk;
    }
    timer.stop();

    if (success && _at.get_last_error() == NSAPI_ERROR_OK) {
        return size - count;
    }

    return _at.get_last_error();
}

nsapi_size_or_error_t TELIT_ME910_CellularStack::socket_recvfrom_impl(CellularSocket *socket, SocketAddress *address,
                                                                      void *buffer, nsapi_size_t size)
{
    tr_debug("TELIT_ME910_CellularStack::socket_recvfrom_impl()");

    nsapi_size_or_error_t nsapi_error_size = NSAPI_ERROR_DEVICE_ERROR;
    bool success = true;
    nsapi_size_t read_blk;
    nsapi_size_t count = 0;
    nsapi_size_t srecv_size;
    Timer timer;
    int port = -1;
    char ip_address[NSAPI_IP_SIZE + 1];

    if (socket->pending_bytes == 0) {
        if (socket->tls_socket && !g_sslsring_en) {
            // SSLSRING is not supported, so loop until the data is ready
            uint32_t timeout = 0;
            do {
                uint32_t sleep_time = 1000; // Default to 1s timeout
                if ((sleep_time + timeout) > MBED_CONF_TELIT_ME910_HTTPS_RECV_TIMEOUT) {
                    sleep_time = MBED_CONF_TELIT_ME910_HTTPS_RECV_TIMEOUT - timeout;
                }
                _at.cmd_start_stop("#SSLI", "=", "%d", 1);
                _at.resp_start("#SSLI:");
                _at.skip_param(3); // Skip SSID, Data Sent, and Data Recieve
                socket->pending_bytes = _at.read_int();
                _at.resp_stop();
                rtos::ThisThread::sleep_for(std::chrono::milliseconds(sleep_time));
                timeout += sleep_time;
                if (!sleep_time) {
                    break;
                }
            } while ((socket->pending_bytes == 0) &&
                    (timeout <= MBED_CONF_TELIT_ME910_HTTPS_RECV_TIMEOUT));
        } else {
            _at.process_oob(); // check for SRING URC
        }
        if (socket->pending_bytes == 0) {
            tr_debug("Socket %d recv would block", socket->id);
            return NSAPI_ERROR_WOULD_BLOCK;
        }
    }

    timer.start();
    while (success && (size > 0)) {
        read_blk = ME910_MAX_RECV_SIZE;
        if (read_blk > size) {
            read_blk = size;
        }
        if (socket->pending_bytes > 0) {
            if (socket->proto == NSAPI_TCP) {
                if (socket->tls_socket) {
                    _at.cmd_start_stop("#SSLRECV", "=", "%d%d", socket->id + 1, read_blk);
                } else {
                    _at.cmd_start_stop("#SRECV", "=", "%d%d", socket->id + 1, read_blk);
                }
            } else {
                _at.cmd_start_stop("#SRECV", "=", "%d%d%d", socket->id + 1, read_blk, 1);
            }

            if (socket->tls_socket) {
                _at.resp_start("#SSLRECV:");
            } else {
                _at.resp_start("#SRECV:");
            }

            if (socket->proto == NSAPI_UDP) {
                // UDP has remote_IP and remote_port parameters
                _at.read_string(ip_address, sizeof(ip_address));
                port = _at.read_int();

                // Skip connId
                _at.skip_param();

                srecv_size = _at.read_int();
                _at.read_int();
                if (srecv_size > size) {
                    srecv_size = size;
                }

                _at.read_bytes((uint8_t *)buffer + count, srecv_size);
            } else {
                // Skip connId if it is a TCP socket
                if (!socket->tls_socket) {
                    _at.skip_param();
                }

                srecv_size = _at.read_int();
                if (srecv_size > size) {
                    srecv_size = size;
                }

                _at.read_bytes((uint8_t *)buffer + count, srecv_size);
            }
            _at.resp_stop();

            if (srecv_size > socket->pending_bytes) {
                socket->pending_bytes = 0;
            } else {
                socket->pending_bytes -= srecv_size;
            }

            if (srecv_size > 0) {
                count += srecv_size;
                size -= srecv_size;
            } else {
                // read() should not fail
                success = false;
            }
        } else if (timer.elapsed_time() < socket_timeout) {
            // Wait for URCs
            _at.process_oob();
        } else {
            if (count == 0) {
                // Timeout with nothing received
                success = false;
            }
            break;
        }
    }
    timer.stop();

    if (!count || (_at.get_last_error() != NSAPI_ERROR_OK)) {
        return NSAPI_ERROR_WOULD_BLOCK;
    } else {
        nsapi_error_size = count;
    }

    if (success && address) {
        address->set_ip_address(ip_address);
        address->set_port(port);
    }

    return nsapi_error_size;
}

#if defined(MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET) && (MBED_CONF_NSAPI_OFFLOAD_TLSSOCKET)

nsapi_error_t TELIT_ME910_CellularStack::setsockopt(nsapi_socket_t handle, int level,
                                                    int optname, const void *optval, unsigned optlen)
{
    CellularSocket *socket = (CellularSocket *)handle;
    nsapi_error_t ret = NSAPI_ERROR_OK;
    int ssl_enabled = 0;
    uint8_t ctrl_z[] = { 0x1A };

    if (level == NSAPI_TLSSOCKET_LEVEL) {
        if (optval) {
            _at.lock();
            switch (optname) {
                case NSAPI_TLSSOCKET_ENABLE: {
                    MBED_ASSERT(optlen == sizeof(bool));
                    bool *enabled = (bool *)optval;
                    if (socket->proto == NSAPI_TCP) {
                        socket->tls_socket = enabled;

                        if (enabled) {
                            _at.at_cmd_discard("#SSLEN", "=", "%d%d", sslctxID, 1);
                            _at.at_cmd_discard("#SSLSECCFG", "=", "%d%d%d", sslctxID, 0, _tls_sec_level);

                            // Check if the device has support for the SSLSRING
                            // #SSLCFG:<SSId>,<cid>,<pktSz>,<maxTo>,<defTo>,<txTo>,<skipHostMismatch>,
                            // <SSLSRingMode>,<noCarrierMode>,<equalizeTx>
                            _at.cmd_start_stop("#SSLCFG", "=?");
                            _at.resp_start("#SSLCFG:");
                            _at.skip_param(7);
                            char buf[6] = {0};
                            auto len = _at.read_string(buf, 6);
                            if ((len > 0) && (len == 3)) {
                                // SSLSRING only has support for one value;
                                // check if that value is 1 (not supporting
                                // chunk format currently)
                                if (buf[1] == '1') {
                                    g_sslsring_en = 1;
                                } else {
                                    g_sslsring_en = 0;
                                }
                            } else if ((len > 0) && (len == 5)) {
                                // Check if we have the full range otherwise,
                                // check the individual numbers for 1
                                if (buf[2] == '-') {
                                    g_sslsring_en = 1;
                                } else if (buf[1] == '1' || buf[3] == '1') {
                                    g_sslsring_en = 1;
                                } else {
                                    g_sslsring_en = 0;
                                }
                            } else {
                                g_sslsring_en = 0;
                            }
                            _at.resp_stop(); // Finish processing the response

                            _at.at_cmd_discard("#SSLCFG", "=", "%d%d%d%d%d%d%d%d",
                                               sslctxID,
                                               _cid,            // PDP context ID
                                               0,               // Packet size (0 is default, select automatically)
                                               90,              // Max socket inactivity time (90 is default)
                                               100,             // Default timeout when no timeout is set (100 is default)
                                               50,              // Transmit timeout (50 is default)
                                               1,               // Ignore Host Mismatch alert (default 1)
                                               g_sslsring_en);  // SSLRING configuration (default 0)

                            ret = _at.get_last_error();
                        }
                    } else {
                        tr_error("Trying to set non-TCPSocket as TLSSocket");
                        ret = NSAPI_ERROR_PARAMETER;
                    }
                }
                break;

                case NSAPI_TLSSOCKET_SET_CACERT: {
                    _at.cmd_start_stop("#SSLEN", "?");
                    _at.resp_start("#SSLEN:");

                    // Skip SSId
                    _at.skip_param();

                    ssl_enabled = _at.read_int();
                    _at.resp_stop();

                    if (ssl_enabled == 0) {
                        // SSL not enabled, so enable it
                        _at.at_cmd_discard("#SSLEN", "=", "%d%d", sslctxID, 1);
                    }

                    _at.flush();

                    const char *cacert = (const char *)optval;
                    int cacert_size = strlen(cacert);
                    _at.cmd_start_stop("#SSLSECDATA", "=", "%d%d%d%d", sslctxID,
                                       1, // store data
                                       1, // CA cert
                                       cacert_size);
                    _at.resp_start("> ", true);
                    _at.write_bytes((uint8_t *)cacert, cacert_size);

                    _at.write_bytes(ctrl_z, 1); // Send Ctrl+Z

                    _at.resp_start("\r\nOK", true);
                    _at.resp_stop();

                    ret = _at.get_last_error();

                    // Set sec level to "Manage server authentication" if only cacert is in use
                    if (ret == NSAPI_ERROR_OK && _tls_sec_level == 0) {
                        _tls_sec_level = 1;
                    }
                }
                break;

                case NSAPI_TLSSOCKET_SET_CLCERT: {
                    _at.cmd_start_stop("#SSLEN", "?");
                    _at.resp_start("#SSLEN:");

                    // Skip SSId
                    _at.skip_param();

                    ssl_enabled = _at.read_int();
                    _at.resp_stop();

                    if (ssl_enabled == 0) {
                        // SSL not enabled, so enable it
                        _at.at_cmd_discard("#SSLEN", "=", "%d%d", sslctxID, 1);
                    }

                    _at.flush();

                    const char *clcert = (const char *)optval;
                    int clcert_size = strlen(clcert);
                    _at.cmd_start_stop("#SSLSECDATA", "=", "%d%d%d%d", sslctxID,
                                       1, // store data
                                       0, // client cert
                                       clcert_size);
                    _at.resp_start("> ", true);
                    _at.write_bytes((uint8_t *)clcert, clcert_size);

                    _at.write_bytes(ctrl_z, 1); // Send Ctrl+Z

                    _at.resp_start("\r\nOK", true);
                    _at.resp_stop();

                    ret = _at.get_last_error();

                    // Set sec level to "Manage server and client authentication if requested by the remote server"
                    if (ret == NSAPI_ERROR_OK) {
                        _tls_sec_level = 2;
                    }
                }
                break;

                case NSAPI_TLSSOCKET_SET_CLKEY: {
                    _at.cmd_start_stop("#SSLEN", "?");
                    _at.resp_start("#SSLEN:");

                    // Skip SSId
                    _at.skip_param();

                    ssl_enabled = _at.read_int();
                    _at.resp_stop();

                    if (ssl_enabled == 0) {
                        // SSL not enabled, so enable it
                        _at.at_cmd_discard("#SSLEN", "=", "%d%d", sslctxID, 1);
                    }

                    _at.flush();

                    const char *clkey = (const char *)optval;
                    int clkey_size = strlen(clkey);
                    _at.cmd_start_stop("#SSLSECDATA", "=", "%d%d%d%d", sslctxID,
                                       1, // store data
                                       2, // client key
                                       clkey_size);
                    _at.resp_start("> ", true);
                    _at.write_bytes((uint8_t *)clkey, clkey_size);

                    _at.write_bytes(ctrl_z, 1); // Send Ctrl+Z

                    _at.resp_start("\r\nOK", true);
                    _at.resp_stop();

                    ret = _at.get_last_error();

                    // Set sec level to "Manage server and client authentication if requested by the remote server"
                    if (ret == NSAPI_ERROR_OK) {
                        _tls_sec_level = 2;
                    }
                }
                break;

                default:
                    tr_error("Unsupported sockopt (%d)", optname);
                    ret = NSAPI_ERROR_UNSUPPORTED;
            }
            _at.unlock();
        } else {
            tr_error("No optval!");
            ret = NSAPI_ERROR_PARAMETER;
        }
    } else {
        tr_warning("Unsupported level (%d)", level);
        ret = NSAPI_ERROR_UNSUPPORTED;
    }

    return ret;
}

#ifdef MBED_CONF_CELLULAR_OFFLOAD_DNS_QUERIES

nsapi_error_t mbed::TELIT_ME910_CellularStack::gethostbyname(const char *host,
        SocketAddress *address, nsapi_version_t version,
        const char *interface_name) {

    (void) interface_name;
    MBED_ASSERT(host);
    MBED_ASSERT(address);

    _at.lock();

    if(_dns_callback) {
        _at.unlock();
        return NSAPI_ERROR_BUSY;
    }

    if(!address->set_ip_address(host)) {
        _at.set_at_timeout(1min);
        _at.set_delimiter(',');
        _at.cmd_start_stop("#QDNS", "=", "%s", host);
        _at.resp_start("#QDNS: ");
        if (!read_qdns(*address, version)) {
            _at.unlock();
            return NSAPI_ERROR_DNS_FAILURE;
        }
        _at.restore_at_timeout();
        _at.set_default_delimiter();
    }

    return _at.unlock_return_error();

}

nsapi_value_or_error_t mbed::TELIT_ME910_CellularStack::gethostbyname_async(
        const char *host, hostbyname_cb_t callback, nsapi_version_t version,
        const char *interface_name) {
    (void) interface_name;
    MBED_ASSERT(host);
    MBED_ASSERT(callback);

    _at.lock();

    if(_dns_callback) {
        _at.unlock();
        return NSAPI_ERROR_BUSY;
    }

    _at.at_cmd_discard("#QDNS", "=", "%s", host);
    if(!_at.get_last_error()) {
        _dns_callback = callback;
        _dns_version = version;
    }

    /* The way it is currently implemented, the ME910 only supports one ongoing DNS request at a time */
    return _at.unlock_return_error() ? NSAPI_ERROR_DNS_FAILURE : 1;

}

nsapi_error_t mbed::TELIT_ME910_CellularStack::gethostbyname_async_cancel(
        int id) {
    _at.lock();
    _dns_callback = nullptr;
    _at.unlock();
    return NSAPI_ERROR_OK;
}

void mbed::TELIT_ME910_CellularStack::urc_qdns() {

    if(!_dns_callback) {
        return;
    }
    SocketAddress address;
    if(read_qdns(address, _dns_version)) {
        _dns_callback(1, &address);
    } else {
        _dns_callback(NSAPI_ERROR_DNS_FAILURE, nullptr);
    }
    _dns_callback = nullptr;

}

bool mbed::TELIT_ME910_CellularStack::read_qdns(SocketAddress &address,
        nsapi_version_t dns_version) {
    /* Format of response: #QDNS: "hostname","ip address"
     * Skip the hostname
     */
    _at.skip_param();
    char ip_address[NSAPI_IP_SIZE];
    _at.read_string(ip_address, sizeof(ip_address));
    if(address.set_ip_address(ip_address)) {
        if(dns_version == NSAPI_UNSPEC || dns_version == address.get_ip_version()) {
            return true;
        }
    }

    return false;
}

#endif

#endif
