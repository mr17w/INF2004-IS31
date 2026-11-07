#include <tk/tkernel.h>
#include <tk/syslib.h>
#include <tm/tmonitor.h>
#include <sys/sysdef.h>

#if TK_SUPPORT_SMP
#include <tk/smp.h>
#endif

#include "display_manager.h"
#include "esp_link.h"
#include "wifi_scan_result.h"

#define ESP_TX_PIN            8U
#define ESP_RX_PIN            9U
#define ESP_BAUD_IBRD         67U
#define ESP_BAUD_FBRD         52U

#define ESP_TASK_STACK_SIZE   3072
#define ESP_QUEUE_COUNT       2
#define ESP_LINE_SIZE         128
#define ESP_LINE_TIMEOUT_MS   8000

#define PING_INTERVAL_MS      500U
#define LOST_AFTER_MS         1500U
#define MONITOR_POLL_MS       50
#define ALIVE_CHECK_MS        1000

#define UART1_BASE            0x40038000UL
#define UART_DR               (UART1_BASE + 0x000U)
#define UART_FR               (UART1_BASE + 0x018U)
#define UART_IBRD             (UART1_BASE + 0x024U)
#define UART_FBRD             (UART1_BASE + 0x028U)
#define UART_LCR_H            (UART1_BASE + 0x02CU)
#define UART_CR               (UART1_BASE + 0x030U)
#define UART_ICR              (UART1_BASE + 0x044U)

#define UART_FR_TXFF          (1U << 5)
#define UART_FR_RXFE          (1U << 4)

#define UART_LCR_H_WLEN_8     (3U << 5)
#define UART_LCR_H_FEN        (1U << 4)

#define UART_CR_RXE           (1U << 9)
#define UART_CR_TXE           (1U << 8)
#define UART_CR_UARTEN        (1U << 0)

typedef enum {
    ESP_COMMAND_SCAN = 0
} EspCommand;

static ID esp_mbf_id;

static EspStatus link_status = ESP_STATUS_UNKNOWN;
static UW last_pong_ms = 0U;
static UW last_ping_ms = 0U;

static char monitor_line[64];
static INT monitor_len = 0;

static UW esp_mbf_buffer[
    ((ESP_QUEUE_COUNT * sizeof(EspCommand)) / sizeof(UW))
    + (ESP_QUEUE_COUNT * 4U)
];

static T_CMBF esp_mbf_config = {
    .mbfatr = TA_TFIFO,
    .bufsz = sizeof(esp_mbf_buffer),
    .maxmsz = sizeof(EspCommand),
    .bufptr = esp_mbf_buffer,
};

static BOOL starts_with(const char *text, const char *prefix)
{
    while (*prefix != '\0') {
        if (*text != *prefix) {
            return FALSE;
        }

        text++;
        prefix++;
    }

    return TRUE;
}

static BOOL same_text(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (*left != *right) {
            return FALSE;
        }

        left++;
        right++;
    }

    return *left == *right;
}

static UW now_ms(void)
{
    SYSTIM time;

    if (tk_get_otm(&time) < E_OK) {
        return 0U;
    }

    /* The lower 32 bits are enough for the short heartbeat intervals here. */
    return time.lo;
}

static void set_link_status(EspStatus status)
{
    if (status == link_status) {
        return;
    }

    link_status = status;
    display_set_esp_status(status);

    if (status == ESP_STATUS_OK) {
        tm_printf((UB *)"[esp] link OK\n");
    } else if (status == ESP_STATUS_LOST) {
        tm_printf((UB *)"[esp] link LOST\n");
    } else {
        tm_printf((UB *)"[esp] link UNKNOWN\n");
    }
}

static BOOL read_number(const char **text, int *value)
{
    const char *p = *text;
    int sign = 1;
    int number = 0;
    BOOL found_digit = FALSE;

    if (*p == '-') {
        sign = -1;
        p++;
    }

    while (*p >= '0' && *p <= '9') {
        found_digit = TRUE;
        number = (number * 10) + (*p - '0');
        p++;
    }

    if (!found_digit) {
        return FALSE;
    }

    *value = number * sign;
    *text = p;

    return TRUE;
}

static BOOL parse_ap_line(const char *line, WifiAccessPoint *ap)
{
    const char *p;
    int i = 0;

    if (!starts_with(line, "AP,")) {
        return FALSE;
    }

    p = line + 3;

    while (*p != '\0' && *p != ',') {
        if (i < WIFI_MAX_SSID_LEN) {
            ap->ssid[i++] = *p;
        }

        p++;
    }

    ap->ssid[i] = '\0';

    if (*p != ',') {
        return FALSE;
    }

    p++;

    if (!read_number(&p, &ap->rssi)) {
        return FALSE;
    }

    if (*p != ',') {
        return FALSE;
    }

    p++;

    if (!read_number(&p, &ap->channel)) {
        return FALSE;
    }

    return TRUE;
}

static void clear_scan_result(WifiScanResult *result)
{
    int i;

    result->success = 0;
    result->count = 0;
    result->scan_ms = 0;
    result->error = WIFI_SCAN_ERROR_NONE;

    for (i = 0; i < WIFI_MAX_APS; i++) {
        result->aps[i].ssid[0] = '\0';
        result->aps[i].rssi = 0;
        result->aps[i].channel = 0;
    }
}

static void esp_uart_init(void)
{
    clr_w(RESETS_RESET, RESETS_RESET_UART1);

    while ((in_w(RESETS_RESET_DONE) & RESETS_RESET_UART1) == 0U) {
    }

    out_w(GPIO_CTRL(ESP_TX_PIN), GPIO_CTRL_FUNCSEL_UART);
    out_w(GPIO_CTRL(ESP_RX_PIN), GPIO_CTRL_FUNCSEL_UART);

    out_w(GPIO(ESP_TX_PIN), GPIO_IE | GPIO_DRIVE_4MA | GPIO_SHEMITT);
    out_w(GPIO(ESP_RX_PIN), GPIO_IE | GPIO_DRIVE_4MA | GPIO_SHEMITT);

    out_w(UART_CR, 0U);
    out_w(UART_IBRD, ESP_BAUD_IBRD);
    out_w(UART_FBRD, ESP_BAUD_FBRD);
    out_w(UART_LCR_H, UART_LCR_H_WLEN_8 | UART_LCR_H_FEN);
    out_w(UART_ICR, 0x7FFU);
    out_w(UART_CR, UART_CR_RXE | UART_CR_TXE | UART_CR_UARTEN);
}

static BOOL esp_uart_readable(void)
{
    return (in_w(UART_FR) & UART_FR_RXFE) == 0U;
}

static void esp_uart_putc(char c)
{
    while ((in_w(UART_FR) & UART_FR_TXFF) != 0U) {
        tk_dly_tsk(1);
    }

    out_w(UART_DR, (UW)(UB)c);
}

static void esp_uart_write(const char *text)
{
    while (*text != '\0') {
        esp_uart_putc(*text++);
    }
}

static void esp_uart_drain(void)
{
    while (esp_uart_readable()) {
        (void)in_w(UART_DR);
    }
}

static BOOL esp_read_line(char *buffer, INT size, INT timeout_ms)
{
    INT count = 0;
    INT waited = 0;

    while (waited < timeout_ms) {
        while (esp_uart_readable()) {
            char c = (char)(in_w(UART_DR) & 0xFFU);

            if (c == '\n') {
                buffer[count] = '\0';
                return TRUE;
            }

            if (c != '\r' && count < size - 1) {
                buffer[count++] = c;
            }
        }

        tk_dly_tsk(1);
        waited++;
    }

    buffer[count] = '\0';
    return FALSE;
}

static void monitor_process_char(char c, UW now)
{
    if (c == '\r') {
        return;
    }

    if (c == '\n') {
        monitor_line[monitor_len] = '\0';

        if (same_text(monitor_line, "PONG") ||
            same_text(monitor_line, "READY")) {
            last_pong_ms = now;
            set_link_status(ESP_STATUS_OK);
        }

        monitor_len = 0;
        return;
    }

    if (monitor_len < (INT)sizeof(monitor_line) - 1) {
        monitor_line[monitor_len++] = c;
    }
}

static void esp_monitor_poll(void)
{
    UW now = now_ms();

    while (esp_uart_readable()) {
        char c = (char)(in_w(UART_DR) & 0xFFU);
        monitor_process_char(c, now);
    }

    if ((UW)(now - last_ping_ms) >= PING_INTERVAL_MS) {
        last_ping_ms = now;
        esp_uart_write("PING\n");
    }

    if (link_status != ESP_STATUS_LOST &&
        (UW)(now - last_pong_ms) >= LOST_AFTER_MS) {
        set_link_status(ESP_STATUS_LOST);
    }
}

static BOOL esp_alive_check(void)
{
    char line[64];
    INT waited = 0;
    UW now;

    esp_uart_write("PING\n");

    while (waited < ALIVE_CHECK_MS) {
        if (esp_read_line(line, (INT)sizeof(line), 100)) {
            if (same_text(line, "PONG")) {
                now = now_ms();
                last_pong_ms = now;
                last_ping_ms = now;
                monitor_len = 0;
                set_link_status(ESP_STATUS_OK);
                return TRUE;
            }
        }

        waited += 100;
    }

    set_link_status(ESP_STATUS_LOST);
    return FALSE;
}

static void run_scan(void)
{
    char line[ESP_LINE_SIZE];
    WifiScanResult result;
    BOOL finished = FALSE;
    ER er;
    UW scan_start;
    UW now;

    clear_scan_result(&result);

    tm_printf((UB *)"[esp] scan requested\n");

    /* clear unfinished/start-up UART data before the real scan */
    esp_uart_write("\n");
    tk_dly_tsk(300);
    esp_uart_drain();
    monitor_len = 0;

    /* check that the ESP32 is connected before starting */
    if (!esp_alive_check()) {
        result.error = WIFI_SCAN_ERROR_ESP_LOST;

        tm_printf((UB *)"[esp] scan cancelled: no PONG\n");

        er = display_show_wifi_results(&result);

        if (er < E_OK) {
            tm_printf(
                (UB *)"[esp] result display failed: %d\n",
                er
            );
        }

        return;
    }

    scan_start = now_ms();

    /*
     * Do not use the normal 1.5 second heartbeat-loss rule while a scan is
     * active. A Wi-Fi scan can legitimately take longer than that.
     */
    esp_uart_write("SCAN\n");

    while ((UW)(now_ms() - scan_start) < ESP_LINE_TIMEOUT_MS) {
        if (!esp_read_line(line, ESP_LINE_SIZE, 100)) {
            continue;
        }

        now = now_ms();

        /* any proper ESP reply proves the link is still alive */
        if (line[0] != '\0') {
            last_pong_ms = now;
            set_link_status(ESP_STATUS_OK);
        }

        if (starts_with(line, "AP,")) {
            tm_printf((UB *)"[esp] %s\n", line);

            if (result.count < WIFI_MAX_APS) {
                if (parse_ap_line(
                        line,
                        &result.aps[result.count]
                    )) {
                    result.count++;
                }
            }

            continue;
        }

        if (starts_with(line, "END")) {
            result.success = 1;
            result.error = WIFI_SCAN_ERROR_NONE;
            result.scan_ms = (int)(now - scan_start);
            finished = TRUE;

            tm_printf(
                (UB *)"[esp] scan complete: %d APs in %d ms\n",
                result.count,
                result.scan_ms
            );

            break;
        }

        if (starts_with(line, "ERR,BUSY")) {
            result.error = WIFI_SCAN_ERROR_BUSY;
            result.scan_ms = (int)(now - scan_start);
            finished = TRUE;

            tm_printf((UB *)"[esp] ERR,BUSY\n");

            break;
        }

        if (starts_with(line, "ERR,SCAN_FAILED")) {
            result.error = WIFI_SCAN_ERROR_SCAN_FAILED;
            result.scan_ms = (int)(now - scan_start);
            finished = TRUE;

            tm_printf((UB *)"[esp] ERR,SCAN_FAILED\n");

            break;
        }

        if (starts_with(line, "ERR")) {
            result.error = WIFI_SCAN_ERROR_PROTOCOL;
            result.scan_ms = (int)(now - scan_start);
            finished = TRUE;

            tm_printf((UB *)"[esp] %s\n", line);

            break;
        }

        if (same_text(line, "PONG") ||
            same_text(line, "READY")) {
            continue;
        }

        if (line[0] != '\0') {
            tm_printf((UB *)"[esp] %s\n", line);
        }
    }

    /*
     * If the whole scan times out, only then check whether the ESP32 itself
     * disappeared or whether this was just a scan timeout.
     */
    if (!finished) {
        now = now_ms();
        result.scan_ms = (int)(now - scan_start);

        tm_printf(
            (UB *)"[esp] scan timed out after %d ms\n",
            result.scan_ms
        );

        if (esp_alive_check()) {
            result.error = WIFI_SCAN_ERROR_TIMEOUT;
        } else {
            result.error = WIFI_SCAN_ERROR_ESP_LOST;
        }
    }

    /* restart the normal idle heartbeat timing after the scan */
    now = now_ms();
    last_ping_ms = now;

    if (result.success) {
        last_pong_ms = now;
        set_link_status(ESP_STATUS_OK);
    }

    tm_printf(
        (UB *)"[esp] result -> success=%d count=%d error=%d scan_ms=%d\n",
        result.success,
        result.count,
        result.error,
        result.scan_ms
    );

    er = display_show_wifi_results(&result);

    if (er < E_OK) {
        tm_printf(
            (UB *)"[esp] result display failed: %d\n",
            er
        );
    }
}

static void esp_task(INT stacd, void *exinf)
{
    EspCommand command;
    INT size;
    UW now;

    (void)stacd;
    (void)exinf;

    now = now_ms();
    last_pong_ms = now;
    last_ping_ms = now - PING_INTERVAL_MS;

    display_set_esp_status(ESP_STATUS_UNKNOWN);

    tm_printf((UB *)"[esp] UART1 ready on GP8/GP9\n");

    while (1) {
        size = tk_rcv_mbf(
            esp_mbf_id,
            &command,
            MONITOR_POLL_MS
        );

        if (size == (INT)sizeof(EspCommand)) {
            if (command == ESP_COMMAND_SCAN) {
                run_scan();
            }
        }

        esp_monitor_poll();
    }
}

static T_CTSK esp_task_config = {
    .itskpri = 7,
    .stksz = ESP_TASK_STACK_SIZE,
    .task = esp_task,
#if TK_SUPPORT_SMP
    .tskatr = TA_HLNG | TA_RNG3 | TA_ASSPRC,
    .assprc = TP_PRC2,
#else
    .tskatr = TA_HLNG | TA_RNG3,
#endif
};

ER esp_link_init(void)
{
    ID task_id;

    esp_uart_init();

    esp_mbf_id = tk_cre_mbf(&esp_mbf_config);

    if (esp_mbf_id <= E_OK) {
        return (ER)esp_mbf_id;
    }

    task_id = tk_cre_tsk(&esp_task_config);

    if (task_id <= E_OK) {
        return (ER)task_id;
    }

    return tk_sta_tsk(task_id, 0);
}

ER esp_link_request_scan(void)
{
    EspCommand command = ESP_COMMAND_SCAN;

    return tk_snd_mbf(
        esp_mbf_id,
        &command,
        (INT)sizeof(EspCommand),
        100
    );
}
