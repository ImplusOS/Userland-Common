/*
 * WifiProbe -- a headless Wi-Fi bring-up and link test, reported on COM1.
 *
 * On a desktop the Wi-Fi panel issues the scan and the connect (the
 * firmware download and the station bring-up run on their own, from the
 * network poll). None of that can be watched in a headless QEMU run or on
 * a machine whose display does not come up, so this program does what the
 * user would do through the panel, in order, and prints every step:
 *
 *   0. wait 30 s without any Wi-Fi call: the driver has to load its
 *      firmware and bring the station up by itself (network poll), as it
 *      must when nobody opens the panel -- then check it is READY
 *   1. poll wifi_get_status() until the adapter reports READY
 *      (firmware loaded, station interface up)
 *   2. scan, and list what was found
 *   3. connect to the configured network and wait for ASSOCIATED
 *      (for WPA2 that means the 4-way handshake finished and keys are in)
 *   4. wait for a DHCP lease (the kernel starts DHCP on link-up)
 *   5. UDP echo (port 7) and TCP echo (port 7) to the DHCP server's DNS
 *      address, which on the simulator is the access point itself
 *
 * and ends with one "[wifiprobe] RESULT: PASS" or "RESULT: FAIL <step>"
 * line, which is what Tools/AIC8800Sim/run_qemu.sh waits for.
 *
 * The network comes from /Userland/wifiprobe.conf when it exists
 *   ssid=<name>
 *   psk=<passphrase>      (omit for an open network)
 * and otherwise defaults to the AIC8800 simulator's access point.
 *
 * Autostart it:
 *   make image_livecd AUTOSTART=/Userland/WifiProbe/WifiProbe.ELF
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "File.h"
#include "Network.h"
#include "Process.h"
#include "Serial.h"
#include "WiFi.h"

#define CONF_PATH          "/Userland/wifiprobe.conf"
#define DEFAULT_SSID       "ImplusSim"
#define DEFAULT_PSK        "sim-passphrase-8800"

#define HANDS_OFF_MS       30000u
#define READY_TIMEOUT_MS   180000u
#define SCAN_WINDOW_MS     4000u
#define ASSOC_TIMEOUT_MS   30000u
#define DHCP_TIMEOUT_MS    30000u
#define ECHO_TIMEOUT_MS    5000u
#define POLL_MS            100u

#define ECHO_PORT          7u
#define UDP_LOCAL_PORT     40007u

static char g_ssid[WIFI_SSID_MAX + 1u];
static char g_psk[64];
static bool g_have_psk;

static void say(const char *s)
{
    serial_write_string("[wifiprobe] ");
    serial_write_string(s);
    serial_write_string("\n");
}

static const char *state_name(uint32_t st)
{
    switch (st) {
    case WIFI_STATE_NO_ADAPTER:       return "NO_ADAPTER";
    case WIFI_STATE_ADAPTER_ATTACHED: return "ADAPTER_ATTACHED";
    case WIFI_STATE_FIRMWARE_LOADING: return "FIRMWARE_LOADING";
    case WIFI_STATE_FIRMWARE_FAILED:  return "FIRMWARE_FAILED";
    case WIFI_STATE_READY:            return "READY";
    case WIFI_STATE_SCANNING:         return "SCANNING";
    case WIFI_STATE_CONNECTING:       return "CONNECTING";
    case WIFI_STATE_ASSOCIATED:       return "ASSOCIATED";
    case WIFI_STATE_CONNECT_FAILED:   return "CONNECT_FAILED";
    default:                          return "?";
    }
}

static void fail(const char *step)
{
    char line[128];
    snprintf(line, sizeof(line), "RESULT: FAIL %s", step);
    say(line);
    process_exit(1);
}

static void ip_str(uint32_t ip, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", (unsigned)((ip >> 24) & 0xFFu), (unsigned)((ip >> 16) & 0xFFu),
             (unsigned)((ip >> 8) & 0xFFu), (unsigned)(ip & 0xFFu));
}

static void conf_value(const char *line, size_t len, const char *key, char *dst, size_t dst_size,
                       bool *seen)
{
    size_t klen = strlen(key);
    if (len <= klen || memcmp(line, key, klen) != 0 || line[klen] != '=') {
        return;
    }
    size_t vlen = len - klen - 1u;
    if (vlen >= dst_size) {
        vlen = dst_size - 1u;
    }
    memcpy(dst, line + klen + 1u, vlen);
    dst[vlen] = '\0';
    if (seen != NULL) {
        *seen = true;
    }
}

static void load_conf(void)
{
    snprintf(g_ssid, sizeof(g_ssid), "%s", DEFAULT_SSID);
    snprintf(g_psk, sizeof(g_psk), "%s", DEFAULT_PSK);
    g_have_psk = true;

    int32_t fd = file_open(CONF_PATH, 0);
    if (fd < 0) {
        return;
    }
    char buf[256];
    int64_t n = file_read(fd, buf, sizeof(buf) - 1u);
    file_close(fd);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';

    bool psk_seen = false;
    size_t pos = 0u;
    while (pos < (size_t)n) {
        size_t end = pos;
        while (end < (size_t)n && buf[end] != '\n' && buf[end] != '\r') {
            ++end;
        }
        conf_value(&buf[pos], end - pos, "ssid", g_ssid, sizeof(g_ssid), NULL);
        conf_value(&buf[pos], end - pos, "psk", g_psk, sizeof(g_psk), &psk_seen);
        pos = end + 1u;
    }
    g_have_psk = psk_seen && g_psk[0] != '\0';
    say("network taken from " CONF_PATH);
}

/* Polls the status (which is also what steps the driver) until `want` or a
 * terminal failure; logs every state change. */
static bool wait_state(uint32_t want, uint32_t timeout_ms, wifi_status_t *st)
{
    uint64_t start = get_uptime_ms();
    uint32_t last = 0xFFFFFFFFu;
    for (;;) {
        wifi_get_status(st);
        if (st->state != last) {
            char line[160];
            snprintf(line, sizeof(line), "t=%llums state %s%s%s",
                     (unsigned long long)(get_uptime_ms() - start), state_name(st->state),
                     st->detail[0] ? " detail=" : "", st->detail);
            say(line);
            last = st->state;
        }
        if (st->state == want) {
            return true;
        }
        if (st->state == WIFI_STATE_FIRMWARE_FAILED ||
            (want == WIFI_STATE_ASSOCIATED && st->state == WIFI_STATE_CONNECT_FAILED)) {
            return false;
        }
        if (get_uptime_ms() - start >= timeout_ms) {
            return false;
        }
        sleep_ms(POLL_MS);
    }
}

static bool udp_echo(uint32_t server)
{
    static const char msg[] = "wifiprobe-udp-echo";
    uint8_t buf[256];

    if (udp_bind_port(UDP_LOCAL_PORT) < 0) {
        say("udp bind failed");
        return false;
    }
    bool ok = false;
    for (uint32_t attempt = 0u; attempt < 3u && !ok; ++attempt) {
        if (!udp_send(server, UDP_LOCAL_PORT, ECHO_PORT, msg, (uint16_t)(sizeof(msg) - 1u))) {
            say("udp send not accepted yet (gateway ARP pending); will retry");
        }
        uint64_t start = get_uptime_ms();
        while (get_uptime_ms() - start < ECHO_TIMEOUT_MS / 3u) {
            int32_t n = udp_recv(UDP_LOCAL_PORT, buf, sizeof(buf));
            if (n >= 8) {
                uint32_t len = (uint32_t)buf[6] | ((uint32_t)buf[7] << 8);
                if (len == sizeof(msg) - 1u && memcmp(&buf[8], msg, len) == 0) {
                    ok = true;
                    break;
                }
            }
            sleep_ms(20u);
        }
    }
    udp_unbind_port(UDP_LOCAL_PORT);
    return ok;
}

static bool tcp_echo(uint32_t server)
{
    static const char msg[] = "wifiprobe-tcp-echo";
    char buf[64];
    int32_t c = tcp_connect(server, ECHO_PORT, 40008u);
    if (c < 0) {
        say("tcp_connect refused");
        return false;
    }
    uint64_t start = get_uptime_ms();
    while (tcp_get_state(c) != TCP_STATE_ESTABLISHED) {
        if (get_uptime_ms() - start >= ECHO_TIMEOUT_MS) {
            say("tcp handshake timed out");
            tcp_close(c);
            return false;
        }
        sleep_ms(20u);
    }
    if (tcp_send(c, msg, (uint16_t)(sizeof(msg) - 1u)) != (int32_t)(sizeof(msg) - 1u)) {
        say("tcp send failed");
        tcp_close(c);
        return false;
    }
    uint32_t got = 0u;
    start = get_uptime_ms();
    while (got < sizeof(msg) - 1u && get_uptime_ms() - start < ECHO_TIMEOUT_MS) {
        int32_t n = tcp_recv(c, buf + got, (uint16_t)(sizeof(buf) - got));
        if (n > 0) {
            got += (uint32_t)n;
        } else {
            sleep_ms(20u);
        }
    }
    tcp_close(c);
    return got == sizeof(msg) - 1u && memcmp(buf, msg, got) == 0;
}

void _start(void)
{
    wifi_status_t st;
    char line[160];

    say("start");
    load_conf();

    /* 0. Hands off: no Wi-Fi call at all for a while. The driver must load
     * its firmware and bring the station up on its own (from the network
     * poll), exactly as when nobody opens the Wi-Fi panel. */
    say("hands-off wait: no Wi-Fi calls for 30 s");
    sleep_ms(HANDS_OFF_MS);
    wifi_get_status(&st);
    snprintf(line, sizeof(line), "after hands-off wait: state %s%s%s", state_name(st.state),
             st.detail[0] ? " detail=" : "", st.detail);
    say(line);
    if (st.state != WIFI_STATE_READY) {
        fail("autonomous-bring-up");
    }

    /* 1. firmware + station bring-up */
    if (!wait_state(WIFI_STATE_READY, READY_TIMEOUT_MS, &st)) {
        fail("bring-up");
    }
    snprintf(line, sizeof(line), "adapter ready, mac %02x:%02x:%02x:%02x:%02x:%02x", st.mac[0],
             st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
    say(line);

    /* 2. scan */
    if (!wifi_scan_start()) {
        fail("scan-start");
    }
    uint64_t start = get_uptime_ms();
    while (get_uptime_ms() - start < SCAN_WINDOW_MS) {
        wifi_get_status(&st);
        if (st.state == WIFI_STATE_READY && get_uptime_ms() - start > 500u) {
            break; /* scan finished */
        }
        sleep_ms(POLL_MS);
    }
    static wifi_scan_result_t results[WIFI_MAX_SCAN_RESULTS];
    uint32_t count = wifi_get_scan_results(results, WIFI_MAX_SCAN_RESULTS);
    bool target_seen = false;
    for (uint32_t i = 0u; i < count; ++i) {
        snprintf(line, sizeof(line), "scan: %-24s %4d dBm %s", results[i].ssid,
                 (int)results[i].rssi_dbm,
                 results[i].security == WIFI_SECURITY_OPEN      ? "open"
                 : results[i].security == WIFI_SECURITY_WPA_PSK ? "wpa2-psk"
                                                                : "unsupported");
        say(line);
        if (strcmp(results[i].ssid, g_ssid) == 0) {
            target_seen = true;
        }
    }
    snprintf(line, sizeof(line), "scan found %u network(s)", (unsigned)count);
    say(line);
    if (!target_seen) {
        fail("target-not-in-scan");
    }

    /* 3. associate */
    snprintf(line, sizeof(line), "connecting to \"%s\" (%s)", g_ssid, g_have_psk ? "wpa2-psk" : "open");
    say(line);
    if (!wifi_connect(g_ssid, g_have_psk ? g_psk : NULL)) {
        fail("connect-request");
    }
    if (!wait_state(WIFI_STATE_ASSOCIATED, ASSOC_TIMEOUT_MS, &st)) {
        fail("associate");
    }

    /* 4. DHCP: the kernel starts it on link-up; the lease's DNS option is
     * the one value userland can read back. */
    uint32_t server = 0u;
    start = get_uptime_ms();
    while ((server = net_get_dhcp_dns_server()) == 0u) {
        if (get_uptime_ms() - start >= DHCP_TIMEOUT_MS) {
            fail("dhcp");
        }
        wifi_get_status(&st);
        if (st.state != WIFI_STATE_ASSOCIATED) {
            fail("link-lost-before-dhcp");
        }
        sleep_ms(POLL_MS);
    }
    ip_str(server, line, sizeof(line));
    char line2[160];
    snprintf(line2, sizeof(line2), "dhcp lease after %llums, dns/server %s",
             (unsigned long long)(get_uptime_ms() - start), line);
    say(line2);

    /* 5. traffic */
    if (!udp_echo(server)) {
        fail("udp-echo");
    }
    say("udp echo ok");
    if (!tcp_echo(server)) {
        fail("tcp-echo");
    }
    say("tcp echo ok");

    say("RESULT: PASS");
    process_exit(0);
}
