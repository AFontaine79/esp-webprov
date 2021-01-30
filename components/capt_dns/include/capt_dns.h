/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 *
 * modified for ESP32 by Cornelis
 * Cornelis' version is available at: https://github.com/cornelis-61/esp32_Captdns
 *
 * modified for ESP32 v4.2 by Aaron Fontaine
 * Additional modifications by Aaron Fontaine
 *  - Fixed IP taken from network interface provided to start function.
 *  - Captive DNS operation restricted to interface provided to start function.
 *  - Ability to stop the DNS server.
 * ----------------------------------------------------------------------------
 */

#ifndef CAPT_DNS_H
#define CAPT_DNS_H

#include <esp_netif.h>

esp_err_t capt_dns_start(esp_netif_t* softap_netif_handle);

void capt_dns_stop(void);

#endif
