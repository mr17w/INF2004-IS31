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
#define ESP_LINE_TIMEOUT_MS   12000

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
#define UART_IMSC           (UART1_BASE + 0x038U)
#define UART_MIS            (UART1_BASE + 0x040U)
#define UART_RSR            (UART1_BASE + 0x004U)
#define UART_IFLS           (UART1_BASE + 0x034U)

#define UART1_INTNO         21U
#define UART_RX_RING_SIZE   1024U
#define UART_RX_RING_MASK   (UART_RX_RING_SIZE - 1U)
#define UART_INT_MASK       ((1U << 4) | (1U << 6) | (1U << 7) | \
                             (1U << 8) | (1U << 9) | (1U << 10))

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

/* ISR on processor 1 (physical core 0), ESP Task on processor 2. */
static UB uart_rx_ring[UART_RX_RING_SIZE];
static volatile UW uart_rx_head = 0U;
static volatile UW uart_rx_tail = 0U;
static volatile UW uart_irq_count = 0U;
static volatile UW uart_rx_bytes = 0U;
static volatile UW uart_sw_dropped = 0U;
static volatile UW uart_hw_errors = 0U;

/* One partly received line can continue across task-side timeouts. */
static char uart_partial_line[ESP_LINE_SIZE];
static INT uart_partial_count = 0;

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

    for (i = 0; i < WIFI_MAX_APS; i++) {
        result->aps[i].ssid[0] = '\0';
        result->aps[i].rssi = 0;
        result->aps[i].channel = 0;
    }
}

/* Read buffered bytes, never touch the hardware FIFO outside the ISR. */
static void uart_ring_barrier(void)
{
    /* RP2040: aligned 32-bit head/tail reads are atomic; DMB orders data. */
    __asm__ volatile ("dmb" ::: "memory");
}

static BOOL esp_uart_pop(char *out)
{
    UW tail = uart_rx_tail;
    UW head;

    uart_ring_barrier();
    head = uart_rx_head;

    if (tail == head) {
        return FALSE;
    }

    *out = (char)uart_rx_ring[tail];
    uart_ring_barrier();
    uart_rx_tail = (tail + 1U) & UART_RX_RING_MASK;
    return TRUE;
}

/* Hardware UART1 interrupt, installed on physical core 0 by usermain(). */
static void esp_uart_irq(UINT intno)
{
    UW irq_status = in_w(UART_MIS);
    UW data;

    uart_irq_count++;

    /* Empty the entire 32-byte hardware FIFO on each interrupt. */
    while ((in_w(UART_FR) & UART_FR_RXFE) == 0U) {
        UW head;
        UW tail;
        UW next;

        data = in_w(UART_DR);
        uart_rx_bytes++;

        if ((data & 0xF00U) != 0U) {
            uart_hw_errors++;
            continue;
        }

        head = uart_rx_head;
        uart_ring_barrier();
        tail = uart_rx_tail;
        next = (head + 1U) & UART_RX_RING_MASK;

        if (next == tail) {
            uart_sw_dropped++;
        } else {
            uart_rx_ring[head] = (UB)(data & 0xFFU);
            uart_ring_barrier();
            uart_rx_head = next;
        }
    }

    /* Also log hardware overrun/error interrupts with an empty FIFO. */
    if ((irq_status & ((1U << 10) | (1U << 9) | (1U << 8) | (1U << 7))) != 0U) {
        uart_hw_errors++;
    }

    out_w(UART_RSR, 0U);   /* clear sticky UART receiver errors */
    out_w(UART_ICR, 0x7FFU);
    ClearInt(intno);
}

static ER esp_uart_init(void)
{
    T_DINT dint;
    ER er;

    /* The kernel's sample serial driver owns the default UART1 handler.
       This test replaces that unused UART1 handler on physical core 0.
       Do not open the sample ser1 device at the same time. */
    out_w(UART_IMSC, 0U);
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
    out_w(UART_IFLS, 0U);   /* RX interrupt at 1/8 of FIFO */
    out_w(UART_RSR, 0U);
    out_w(UART_ICR, 0x7FFU);

    dint.intatr = TA_HLNG;
    dint.inthdr = (FP)esp_uart_irq;
    er = tk_def_int(UART1_INTNO, &dint);
    if (er < E_OK) {
        return er;
    }

    ClearInt(UART1_INTNO);
    EnableInt(UART1_INTNO, 2);   /* installed on core 0 */
    out_w(UART_CR, UART_CR_RXE | UART_CR_TXE | UART_CR_UARTEN);
    out_w(UART_IMSC, UART_INT_MASK);

    return E_OK;
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
    char unused;

    while (esp_uart_pop(&unused)) {
    }
    uart_partial_count = 0;
}

/* Task-side line parser reads only the software ring, not UART_DR. */
static BOOL esp_read_line(char *buffer, INT size, INT timeout_ms)
{
    UW start = now_ms();
    char c;
    INT i;

    while ((UW)(now_ms() - start) < (UW)timeout_ms) {
        while (esp_uart_pop(&c)) {
            if (c == '\r') {
                continue;
            }

            if (c == '\n') {
                uart_partial_line[uart_partial_count] = '\0';
                for (i = 0; i < uart_partial_count && i < size - 1; i++) {
                    buffer[i] = uart_partial_line[i];
                }
                buffer[i] = '\0';
                uart_partial_count = 0;
                return TRUE;
            }

            if (uart_partial_count < ESP_LINE_SIZE - 1) {
                uart_partial_line[uart_partial_count++] = c;
            }
        }

        /* Safe now: the interrupt continues receiving while this task sleeps. */
        tk_dly_tsk(1);
    }

    return FALSE;
}

static void monitor_process_char(char c, UW now)
{
    if (c == '\r') {
        return;
    }

    if (c == '\n') {
        monitor_line[monitor_len] = '\0';

        if (same_text(monitor_line, "PONG")) {
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

    {
        char c;
        while (esp_uart_pop(&c)) {
            monitor_process_char(c, now);
        }
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
    BOOL got_reply = FALSE;
    ER er;
    UW now;
    UW scan_start;
    UW before_bytes;
    UW before_sw_dropped;
    UW before_hw_errors;
    UW before_irqs;
    INT ap_lines = 0;
    INT end_seen = 0;

    clear_scan_result(&result);
    tm_printf((UB *)"[esp] scan requested\n");

    esp_uart_write("\n");
    tk_dly_tsk(300);
    esp_uart_drain();
    monitor_len = 0;

    if (!esp_alive_check()) {
        tm_printf((UB *)"[esp] scan cancelled: no PONG\n");
        (void)display_show_wifi_results(&result);
        return;
    }

    before_bytes = uart_rx_bytes;
    before_sw_dropped = uart_sw_dropped;
    before_hw_errors = uart_hw_errors;
    before_irqs = uart_irq_count;

    scan_start = now_ms();
    esp_uart_write("SCAN\n");

    /* One global 12-second deadline, not a new 8-second wait for each line. */
    while ((UW)(now_ms() - scan_start) < ESP_LINE_TIMEOUT_MS) {
        if (!esp_read_line(line, ESP_LINE_SIZE, 100)) {
            continue;
        }

        if (line[0] != '\0') {
            got_reply = TRUE;
        }

        if (starts_with(line, "AP,")) {
            ap_lines++;
            if (result.count < WIFI_MAX_APS &&
                parse_ap_line(line, &result.aps[result.count])) {
                result.count++;
            }
            continue;  /* no slow debug prints while UART burst is arriving */
        }

        if (starts_with(line, "END")) {
            end_seen = 1;
            result.success = 1;
            finished = TRUE;
            break;
        }

        if (starts_with(line, "ERR")) {
            finished = TRUE;
            tm_printf((UB *)"[esp] %s\n", line);
            break;
        }

        if (same_text(line, "PONG") || same_text(line, "READY")) {
            last_pong_ms = now_ms();
            continue;
        }

        tm_printf((UB *)"[esp] unexpected reply: %s\n", line);
    }

    now = now_ms();
    last_ping_ms = now;
    if (result.success) {
        last_pong_ms = now;
        set_link_status(ESP_STATUS_OK);
        tm_printf((UB *)"[esp] scan complete: %d APs\n", result.count);
    } else if (!finished) {
        tm_printf((UB *)"[esp] scan timed out (no END)\n");
        if (!got_reply) {
            set_link_status(ESP_STATUS_LOST);
        }
    }

    /* These counters tell us whether hardware or ring-buffer loss occurred. */
    tm_printf(
        (UB *)"[uart1] irq=%u bytes=%u hw_errors=%u ring_dropped=%u AP_lines=%d END=%d\n",
        uart_irq_count - before_irqs,
        uart_rx_bytes - before_bytes,
        uart_hw_errors - before_hw_errors,
        uart_sw_dropped - before_sw_dropped,
        ap_lines,
        end_seen
    );

    if (result.success) {
        INT i;
        for (i = 0; i < result.count; i++) {
            tm_printf(
                (UB *)"[esp] AP,%s,%d,%d\n",
                result.aps[i].ssid,
                result.aps[i].rssi,
                result.aps[i].channel
            );
        }
    }

    er = display_show_wifi_results(&result);
    if (er < E_OK) {
        tm_printf((UB *)"[esp] result display failed: %d\n", er);
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

    ER er = esp_uart_init();

    if (er < E_OK) {
        return er;
    }

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
