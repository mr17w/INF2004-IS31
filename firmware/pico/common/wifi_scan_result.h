#ifndef WIFI_SCAN_RESULT_H
#define WIFI_SCAN_RESULT_H

#define WIFI_MAX_APS       10
#define WIFI_MAX_SSID_LEN  32

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN + 1];
    int rssi;
    int channel;
} WifiAccessPoint;

typedef struct {
    int success;
    int count;
    WifiAccessPoint aps[WIFI_MAX_APS];
} WifiScanResult;

#endif
