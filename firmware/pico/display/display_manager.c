#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#if TK_SUPPORT_SMP
#include <tk/smp.h>
#endif

#include "display_manager.h"
#include "st7789.h"

#define DISPLAY_STACK_SIZE   4096
#define DISPLAY_QUEUE_COUNT  8

static ID display_mbf_id;
static EspStatus current_esp_status = ESP_STATUS_UNKNOWN;

static UW display_mbf_buffer[
    ((DISPLAY_QUEUE_COUNT * sizeof(DisplayMessage)) / sizeof(UW))
    + (DISPLAY_QUEUE_COUNT * 4U)
];

static T_CMBF display_mbf_config = {
    .mbfatr = TA_TFIFO,
    .bufsz = sizeof(display_mbf_buffer),
    .maxmsz = sizeof(DisplayMessage),
    .bufptr = display_mbf_buffer,
};

static void copy_result(WifiScanResult *dest, const WifiScanResult *src)
{
    int i;
    int j;

    dest->success = src->success;
    dest->count = src->count;

    for (i = 0; i < WIFI_MAX_APS; i++) {
        dest->aps[i].rssi = src->aps[i].rssi;
        dest->aps[i].channel = src->aps[i].channel;

        for (j = 0; j <= WIFI_MAX_SSID_LEN; j++) {
            dest->aps[i].ssid[j] = src->aps[i].ssid[j];

            if (src->aps[i].ssid[j] == '\0') {
                break;
            }
        }

        if (j > WIFI_MAX_SSID_LEN) {
            dest->aps[i].ssid[WIFI_MAX_SSID_LEN] = '\0';
        }
    }
}

static void int_to_text(int value, char *buffer)
{
    char temp[12];
    int negative = 0;
    int count = 0;
    int i = 0;
    unsigned int number;

    if (value < 0) {
        negative = 1;
        number = (unsigned int)(-value);
    } else {
        number = (unsigned int)value;
    }

    if (number == 0U) {
        temp[count++] = '0';
    } else {
        while (number > 0U && count < (int)sizeof(temp) - 1) {
            temp[count++] = (char)('0' + (number % 10U));
            number /= 10U;
        }
    }

    if (negative) {
        buffer[i++] = '-';
    }

    while (count > 0) {
        buffer[i++] = temp[--count];
    }

    buffer[i] = '\0';
}

static void copy_short_ssid(char *dest, const char *src)
{
    int i = 0;

    if (src[0] == '\0') {
        const char hidden[] = "(HIDDEN)";

        while (hidden[i] != '\0' && i < 19) {
            dest[i] = hidden[i];
            i++;
        }

        dest[i] = '\0';
        return;
    }

    while (src[i] != '\0' && i < 19) {
        dest[i] = src[i];
        i++;
    }

    dest[i] = '\0';
}

static void draw_esp_badge(void)
{
    const char *text;
    UH colour;

    st7789_fill_rect(142U, 0U, 98U, 18U, ST7789_BLACK);

    if (current_esp_status == ESP_STATUS_OK) {
        text = "ESP OK";
        colour = ST7789_GREEN;
    } else if (current_esp_status == ESP_STATUS_LOST) {
        text = "ESP LOST";
        colour = ST7789_RED;
    } else {
        text = "ESP ...";
        colour = ST7789_WHITE;
    }

    st7789_draw_text(
        144U,
        4U,
        text,
        colour,
        ST7789_BLACK,
        1U
    );
}

static void show_boot_screen(void)
{
    st7789_fill(ST7789_BLACK);

    st7789_draw_text(
        21U,
        145U,
        "OTTIE READY",
        ST7789_WHITE,
        ST7789_BLACK,
        3U
    );

    draw_esp_badge();
}

static void draw_menu_marker(UH y)
{
    st7789_fill_rect(18U, y + 2U, 4U, 4U, ST7789_WHITE);
    st7789_fill_rect(22U, y + 6U, 4U, 4U, ST7789_WHITE);
    st7789_fill_rect(18U, y + 10U, 4U, 4U, ST7789_WHITE);
}

static void draw_menu_item(UH y, const char *text, BOOL selected)
{
    if (selected) {
        draw_menu_marker(y);
    }

    st7789_draw_text(
        42U,
        y,
        text,
        ST7789_WHITE,
        ST7789_BLACK,
        2U
    );
}

static void show_menu(MenuItem selected)
{
    st7789_fill(ST7789_BLACK);

    st7789_draw_text(
        75U,
        35U,
        "OTTIE",
        ST7789_WHITE,
        ST7789_BLACK,
        3U
    );

    draw_menu_item(105U, "WIFI SCAN", selected == MENU_ITEM_WIFI_SCAN);
    draw_menu_item(145U, "PEER STATUS", selected == MENU_ITEM_PEER_STATUS);
    draw_menu_item(185U, "SYSTEM INFO", selected == MENU_ITEM_SYSTEM_INFO);

    draw_esp_badge();
}

static void show_status_screen(const char *title, const char *status)
{
    st7789_fill(ST7789_BLACK);

    st7789_draw_text(
        18U,
        95U,
        title,
        ST7789_WHITE,
        ST7789_BLACK,
        2U
    );

    st7789_draw_text(
        18U,
        150U,
        status,
        ST7789_WHITE,
        ST7789_BLACK,
        2U
    );

    draw_esp_badge();
}

static void show_wifi_screen(void)
{
    show_status_screen("WIFI SCAN", "SCANNING...");
}

static void show_wifi_results(const WifiScanResult *result)
{
    int i;
    UH y;
    char ssid[20];
    char rssi[12];
    char channel[12];
    char count[12];

    st7789_fill(ST7789_BLACK);

    st7789_draw_text(
        0U,
        4U,
        "WIFI SCAN",
        ST7789_WHITE,
        ST7789_BLACK,
        2U
    );

    if (!result->success) {
        st7789_draw_text(
            18U,
            130U,
            current_esp_status == ESP_STATUS_LOST
                ? "ESP32 LOST"
                : "SCAN FAILED",
            ST7789_RED,
            ST7789_BLACK,
            2U
        );

        st7789_draw_text(
            0U,
            292U,
            "KO = MENU",
            ST7789_WHITE,
            ST7789_BLACK,
            1U
        );

        draw_esp_badge();
        return;
    }

    st7789_draw_text(
        0U,
        28U,
        "SSID",
        ST7789_WHITE,
        ST7789_BLACK,
        1U
    );

    st7789_draw_text(
        130U,
        28U,
        "RSSI",
        ST7789_WHITE,
        ST7789_BLACK,
        1U
    );

    st7789_draw_text(
        178U,
        28U,
        "CH",
        ST7789_WHITE,
        ST7789_BLACK,
        1U
    );

    for (i = 0; i < result->count && i < WIFI_MAX_APS; i++) {
        y = (UH)(48 + (i * 20));

        copy_short_ssid(ssid, result->aps[i].ssid);
        int_to_text(result->aps[i].rssi, rssi);
        int_to_text(result->aps[i].channel, channel);

        st7789_draw_text(
            0U,
            y,
            ssid,
            ST7789_WHITE,
            ST7789_BLACK,
            1U
        );

        st7789_draw_text(
            130U,
            y,
            rssi,
            ST7789_WHITE,
            ST7789_BLACK,
            1U
        );

        st7789_draw_text(
            178U,
            y,
            channel,
            ST7789_WHITE,
            ST7789_BLACK,
            1U
        );
    }

    int_to_text(result->count, count);

    st7789_draw_text(
        0U,
        258U,
        count,
        ST7789_GREEN,
        ST7789_BLACK,
        2U
    );

    st7789_draw_text(
        30U,
        258U,
        "FOUND",
        ST7789_GREEN,
        ST7789_BLACK,
        2U
    );

    st7789_draw_text(
        0U,
        292U,
        "KO = MENU",
        ST7789_WHITE,
        ST7789_BLACK,
        1U
    );

    draw_esp_badge();
}

static void show_peer_screen(void)
{
    show_status_screen("PEER STATUS", "NOT READY");
}

static void show_system_screen(void)
{
    show_status_screen("SYSTEM INFO", "SMP READY");
}

static void display_task(INT stacd, void *exinf)
{
    DisplayMessage message;
    INT size;
    ER er;
    BOOL wifi_screen_active = FALSE;

    (void)stacd;
    (void)exinf;

    er = st7789_init();

    if (er < E_OK) {
        tm_printf((UB *)"[display] lcd init failed: %d\n", er);
        tk_slp_tsk(TMO_FEVR);
    }

    while (1) {
        size = tk_rcv_mbf(display_mbf_id, &message, TMO_FEVR);

        if (size != (INT)sizeof(DisplayMessage)) {
            continue;
        }

        switch (message.event) {
            case DISPLAY_EVENT_BOOT:
                wifi_screen_active = FALSE;
                show_boot_screen();
                break;

            case DISPLAY_EVENT_SHOW_MENU:
                wifi_screen_active = FALSE;
                show_menu(message.menu_item);
                break;

            case DISPLAY_EVENT_WIFI_SCREEN:
                wifi_screen_active = TRUE;
                show_wifi_screen();
                break;

            case DISPLAY_EVENT_WIFI_RESULTS:
                if (wifi_screen_active) {
                    show_wifi_results(&message.wifi_result);
                }
                break;

            case DISPLAY_EVENT_PEER_SCREEN:
                wifi_screen_active = FALSE;
                show_peer_screen();
                break;

            case DISPLAY_EVENT_SYSTEM_SCREEN:
                wifi_screen_active = FALSE;
                show_system_screen();
                break;

            case DISPLAY_EVENT_ESP_STATUS:
                current_esp_status = message.esp_status;
                draw_esp_badge();
                break;

            default:
                break;
        }
    }
}

static T_CTSK display_task_config = {
    .itskpri = 8,
    .stksz = DISPLAY_STACK_SIZE,
    .task = display_task,
#if TK_SUPPORT_SMP
    .tskatr = TA_HLNG | TA_RNG3 | TA_ASSPRC,
    .assprc = TP_PRC1,
#else
    .tskatr = TA_HLNG | TA_RNG3,
#endif
};

ER display_manager_init(void)
{
    ID task_id;

    display_mbf_id = tk_cre_mbf(&display_mbf_config);

    if (display_mbf_id <= E_OK) {
        return (ER)display_mbf_id;
    }

    task_id = tk_cre_tsk(&display_task_config);

    if (task_id <= E_OK) {
        return (ER)task_id;
    }

    return tk_sta_tsk(task_id, 0);
}

ER display_send(DisplayEvent event)
{
    DisplayMessage message = {0};

    message.event = event;
    message.menu_item = MENU_ITEM_WIFI_SCAN;

    return tk_snd_mbf(
        display_mbf_id,
        &message,
        (INT)sizeof(DisplayMessage),
        100
    );
}

ER display_show_menu(MenuItem selected)
{
    DisplayMessage message = {0};

    message.event = DISPLAY_EVENT_SHOW_MENU;
    message.menu_item = selected;

    return tk_snd_mbf(
        display_mbf_id,
        &message,
        (INT)sizeof(DisplayMessage),
        100
    );
}

ER display_show_wifi_results(const WifiScanResult *result)
{
    DisplayMessage message = {0};

    message.event = DISPLAY_EVENT_WIFI_RESULTS;
    copy_result(&message.wifi_result, result);

    return tk_snd_mbf(
        display_mbf_id,
        &message,
        (INT)sizeof(DisplayMessage),
        100
    );
}

ER display_set_esp_status(EspStatus status)
{
    DisplayMessage message = {0};

    message.event = DISPLAY_EVENT_ESP_STATUS;
    message.esp_status = status;

    return tk_snd_mbf(
        display_mbf_id,
        &message,
        (INT)sizeof(DisplayMessage),
        100
    );
}
