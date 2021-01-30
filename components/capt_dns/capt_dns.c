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
 *
 * Note: This code appears to have originally been copied from ESP-IDF UDP server
 * example and modified (license CC0 or Public Domain at user's discretion.)
 * ----------------------------------------------------------------------------
 */

/*
This is a 'captive portal' DNS server: it basically replies with a fixed IP (in this case:
the one of the SoftAP interface of this ESP module) for any and all DNS queries. This can
be used to send mobile phones, tablets etc which connect to the ESP in AP mode directly to
the internal webserver.
*/

#include "capt_dns.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/sockets.h"

#define DNS_LEN  512
#define DNS_PORT 53

static const char* TAG = "capt_dns";

static int _sockFd;
static esp_netif_ip_info_t _ip_info_of_softap;

/* Signal Wi-Fi events on this event-group */
const EventBits_t DNS_SERVER_STOP_REQUESTED_EVENT = BIT0;
const EventBits_t DNS_SERVER_STOP_COMPLETE_EVENT = BIT1;
static EventGroupHandle_t _capt_dns_event_group = NULL;

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint8_t flags;
    uint8_t rcode;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} DnsHeader;

typedef struct __attribute__((packed)) {
    uint8_t len;
    uint8_t data;
} DnsLabel;

typedef struct __attribute__((packed)) {
    // before: label
    uint16_t type;
    uint16_t class;
} DnsQuestionFooter;

typedef struct __attribute__((packed)) {
    // before: label
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t rdlength;
    // after: rdata
} DnsResourceFooter;

typedef struct __attribute__((packed)) {
    uint16_t prio;
    uint16_t weight;
} DnsUriHdr;

#define FLAG_QR (1 << 7)
#define FLAG_AA (1 << 2)
#define FLAG_TC (1 << 1)
#define FLAG_RD (1 << 0)

#define QTYPE_A     1
#define QTYPE_NS    2
#define QTYPE_CNAME 5
#define QTYPE_SOA   6
#define QTYPE_WKS   11
#define QTYPE_PTR   12
#define QTYPE_HINFO 13
#define QTYPE_MINFO 14
#define QTYPE_MX    15
#define QTYPE_TXT   16
#define QTYPE_URI   256

#define QCLASS_IN  1
#define QCLASS_ANY 255
#define QCLASS_URI 256

/**
 * Note: The following replacements for host/network byte order conversion
 * functions since we are using packed structs to access the various DNS query
 * and response fields.
 */

// Function to put unaligned 16-bit network values
static void setn16(void* pp, int16_t n)
{
    char* p = pp;
    *p++ = (n >> 8);
    *p++ = (n & 0xff);
}

// Function to put unaligned 32-bit network values
static void setn32(void* pp, int32_t n)
{
    char* p = pp;
    *p++ = (n >> 24) & 0xff;
    *p++ = (n >> 16) & 0xff;
    *p++ = (n >> 8) & 0xff;
    *p++ = (n & 0xff);
}

// ntohs function for unaligned 16-bit network values
static uint16_t my_ntohs(uint16_t* in)
{
    char* p = (char*)in;
    return ((p[0] << 8) & 0xff00) | (p[1] & 0xff);
}

// Parses the QNAME field of a Question into a C-string containing a dotted domain name.
// Returns pointer to start of next fields in packet (i.e. QTYPE and QCLASS).
static char* label_to_str(char* packet, char* qnamePtr, int packetSz, char* res, int resMaxLen)
{
    int resIdx, currentLabelLen, currentLabelIdx;
    char* endPtr = NULL;

    resIdx = 0;

    // For each label in QNAME field...
    do {
        if ((*qnamePtr & 0xC0) == 0) {
            // Get length of this label and move past length octet.
            currentLabelLen = *qnamePtr++;

            // Add separator period if this is not the first label in qname field.
            if (resIdx < resMaxLen && resIdx != 0)
                res[resIdx++] = '.';

            // Copy the label into the response buffer
            for (currentLabelIdx = 0; currentLabelIdx < currentLabelLen; currentLabelIdx++) {
                if ((qnamePtr - packet) > packetSz) {
                    // Sanity check to ensure we do not run past the end of the DNS Query Request
                    // packet.
                    return NULL;
                }
                if (resIdx < resMaxLen) {
                    // Copy one character
                    res[resIdx++] = *qnamePtr++;
                }
            }
        } else if ((*qnamePtr & 0xC0) == 0xC0) {
            // Compressed label pointer
            // Labels cannot be longer than 63 bytes. A length octet beginning with b11 means we
            // have a 14-bit offset from start of DNS packet from which to read the rest of the
            // QNAME field.
            endPtr = qnamePtr + 2;

            int offset = my_ntohs(((uint16_t*)qnamePtr)) & 0x3FFF;

            // Check if offset points to somewhere outside of the packet
            if (offset > packetSz) {
                return NULL;
            }

            qnamePtr = &packet[offset];
        }

        // check for out-of-bound-ness
        if ((qnamePtr - packet) > packetSz) {
            return NULL;
        }
    } while (*qnamePtr != 0);

    res[resIdx] = 0; // zero-terminate

    // If end-pointer is not null, then we had a compressed field in the Query and qnamePtr is no
    // longer pointing to the end of the same QNAME field. If end-pointer is null, then it was not
    // used and qnamePtr is still referencing the same QNAME field.
    if (endPtr == NULL) {
        endPtr = qnamePtr + 1;
    }

    // Pointer to first byte following QNAME field we were asked to parses
    return endPtr;
}

// Convert a dotted hostname string into a DNS QNAME field.
static char* str_to_label(char* str, char* outBuff, int maxLen)
{
    char* lenPtr = outBuff;       // Pointer to length byte preceding label currently being written.
    char* writePtr = outBuff + 1; // Position in outBuff currently being written.

    while (1) {
        if ((*str == '.') || (*str == 0)) {
            // Finished writing current label
            *lenPtr = ((writePtr - lenPtr) - 1); // Write out the length for this label
            lenPtr = writePtr; // Reposition the pointer to length byte for the next label
            writePtr++;        // Update the write pointer
            if (*str == 0)
                break; // Exit if done
            str++;     // Update the read pointer for the next label
        } else {
            *writePtr++ = *str++; // Copy one byte

            // Check out of bounds
            if ((writePtr - outBuff) > maxLen) {
                return NULL;
            }
        }
    }

    // Set final byte of QNAME field to 0 to indicate completion.
    // (Compression is not used when writing QNAMEs back into the response.)
    *lenPtr = 0;

    // Return pointer to first free byte in response.
    return writePtr;
}

static bool is_stop_pending(void)
{
    EventBits_t bits = xEventGroupGetBits(_capt_dns_event_group);
    if (bits & DNS_SERVER_STOP_REQUESTED_EVENT) {
        return true;
    } else {
        return false;
    }
}

// Receive a DNS packet and maybe send a response back
static void capt_dns_recv(struct sockaddr_in* premote_addr, char* pusrdata, unsigned short length)
{
    char buff[DNS_LEN];
    char reply[DNS_LEN];
    int i;
    char* rend = &reply[length];
    char* p = pusrdata;
    DnsHeader* hdr = (DnsHeader*)p;
    DnsHeader* rhdr = (DnsHeader*)&reply[0];
    p += sizeof(DnsHeader);

    ESP_LOGD(
        TAG,
        "DNS packet: id 0x%X flags 0x%X rcode 0x%X qcnt %d ancnt %d nscount %d arcount %d len %d",
        my_ntohs(&hdr->id), hdr->flags, hdr->rcode, my_ntohs(&hdr->qdcount),
        my_ntohs(&hdr->ancount), my_ntohs(&hdr->nscount), my_ntohs(&hdr->arcount), length);

    // Some sanity checks:
    // TODO: Should an error be returned to the requester if the sanity checks fail?

    if (length > DNS_LEN) {
        // Packet is longer than DNS implementation allows.
        return;
    }

    if (length < sizeof(DnsHeader)) {
        // Packet is too short.
        return;
    }

    if (hdr->ancount || hdr->nscount || hdr->arcount) {
        // This is a reply... but we are the server.
        // Don't know what to do with it.
        return;
    }

    if (hdr->flags & FLAG_TC) {
        // The packet is truncated. We can't work with truncated packets.
        return;
    }

    // Reply is basically the request plus the needed data
    memcpy(reply, pusrdata, length);

    // Set Query Response flag in response header
    rhdr->flags |= FLAG_QR;

    // For each Question in request packet...
    for (i = 0; i < my_ntohs(&hdr->qdcount); i++) {
        // Grab the labels in the QNAME field
        // These, concatenated together with dots, represent the domain name being queried.
        p = label_to_str(pusrdata, p, length, buff, sizeof(buff));
        if (p == NULL) {
            // Invalid request. Return error?
            return;
        }

        DnsQuestionFooter* qf = (DnsQuestionFooter*)p;
        p += sizeof(DnsQuestionFooter);

        ESP_LOGI(TAG, "DNS: Q (type 0x%X class 0x%X) for %s", my_ntohs(&qf->type),
                 my_ntohs(&qf->class), buff);

        if (my_ntohs(&qf->type) == QTYPE_A) {
            // They want to know the IPv4 address of something.
            // Build the response.

            // This is where we institute our captive portal and return our local IP address for
            // everything.

            // Add the label
            rend = str_to_label(buff, rend, sizeof(reply) - (rend - reply));
            if (rend == NULL) {
                return;
            }

            DnsResourceFooter* rf = (DnsResourceFooter*)rend;
            rend += sizeof(DnsResourceFooter);

            setn16(&rf->type, QTYPE_A);
            setn16(&rf->class, QCLASS_IN);
            setn32(&rf->ttl, 0);
            setn16(&rf->rdlength, 4); // Response is IPv4 address, which is 4 bytes

            // Always respond with our IP address
            *rend++ = ip4_addr1(&_ip_info_of_softap.ip);
            *rend++ = ip4_addr2(&_ip_info_of_softap.ip);
            *rend++ = ip4_addr3(&_ip_info_of_softap.ip);
            *rend++ = ip4_addr4(&_ip_info_of_softap.ip);

            setn16(&rhdr->ancount, my_ntohs(&rhdr->ancount) + 1);

            ESP_LOGD(TAG, "IP Address:  %s", inet_ntoa(_ip_info_of_softap.ip));
            ESP_LOGD(TAG, "Added A rec to resp. Resp len is %d", (rend - reply));

        } else if (my_ntohs(&qf->type) == QTYPE_NS) {
            // They are requesting a name server.
            // Basically, we can respond with whatever we want because it will get resolved
            // to our IP address later anyway.

            rend = str_to_label(buff, rend, sizeof(reply) - (rend - reply));

            DnsResourceFooter* rf = (DnsResourceFooter*)rend;
            rend += sizeof(DnsResourceFooter);

            setn16(&rf->type, QTYPE_NS);
            setn16(&rf->class, QCLASS_IN);
            setn16(&rf->ttl, 0);
            setn16(&rf->rdlength, 4);

            // Here is the "whatever we want" part.
            // Our name server is "ns".
            *rend++ = 2;
            *rend++ = 'n';
            *rend++ = 's';
            *rend++ = 0;

            setn16(&rhdr->ancount, my_ntohs(&rhdr->ancount) + 1);

            ESP_LOGD(TAG, "Added NS rec to resp. Resp len is %d", (rend - reply));

        } else if (my_ntohs(&qf->type) == QTYPE_URI) {
            // Give uri to us

            rend = str_to_label(buff, rend, sizeof(reply) - (rend - reply));

            DnsResourceFooter* rf = (DnsResourceFooter*)rend;
            rend += sizeof(DnsResourceFooter);

            DnsUriHdr* uh = (DnsUriHdr*)rend;
            rend += sizeof(DnsUriHdr);

            setn16(&rf->type, QTYPE_URI);
            setn16(&rf->class, QCLASS_URI);
            setn16(&rf->ttl, 0);
            setn16(&rf->rdlength, 4 + 16);

            setn16(&uh->prio, 10);
            setn16(&uh->weight, 1);
            memcpy(rend, "http://esp.nonet", 16);
            rend += 16;
            setn16(&rhdr->ancount, my_ntohs(&rhdr->ancount) + 1);

            ESP_LOGD(TAG, "Added NS rec to resp. Resp len is %d", (rend - reply));
        }
    }

    // Send the response
    ESP_LOGD(TAG, "Sending response");
    sendto(_sockFd, (uint8_t*)reply, rend - reply, 0, (struct sockaddr*)premote_addr,
           sizeof(struct sockaddr_in));
}

static void capt_dns_task(void* pvParameters)
{
    struct sockaddr_in server_addr;
    int ret;
    struct sockaddr_in from;
    socklen_t fromlen;
    char udp_msg[DNS_LEN];

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(DNS_PORT);
    server_addr.sin_len = sizeof(server_addr);

    _sockFd = -1;
    while ((_sockFd == -1) && (!is_stop_pending())) {
        _sockFd = socket(AF_INET, SOCK_DGRAM, 0);
        if (_sockFd == -1) {
            ESP_LOGW(TAG, "capt_dns_task failed to create socket!\nTrying again in 1000ms.");
            vTaskDelay(1000 / portTICK_RATE_MS);
        }
    }

    ret = -1;
    while ((ret != 0) && (!is_stop_pending())) {
        ret = bind(_sockFd, (struct sockaddr*)&server_addr, sizeof(server_addr));
        if (ret != 0) {
            ESP_LOGW(TAG, "capt_dns_task failed to bind sock!\nTrying again in 1000ms.");
            vTaskDelay(1000 / portTICK_RATE_MS);
        }
    }

    // We need socket to be non-blocking so that we can keep checking
    // whether there's been a request to stop the DNS server.
    ret = -1;
    while ((ret != 0) && (!is_stop_pending())) {
        int flags = fcntl(_sockFd, F_GETFL, 0);
        if (flags == -1) {
            continue;
        }
        flags |= O_NONBLOCK;
        ret = fcntl(_sockFd, F_SETFL, flags);
    }

    if (!is_stop_pending()) {
        ESP_LOGI(TAG, "capt_dns initialization complete.");
    }

    while (!is_stop_pending()) {
        memset(&from, 0, sizeof(from));
        fromlen = sizeof(struct sockaddr_in);

        do {
            ret = recvfrom(_sockFd, (uint8_t*)udp_msg, DNS_LEN, 0, (struct sockaddr*)&from,
                           (socklen_t*)&fromlen);
            vTaskDelay(200 / portTICK_RATE_MS);
        } while ((ret == -1) && (errno == EWOULDBLOCK) && (!is_stop_pending()));

        if (ret > 0) {
            // The DNS server sees incoming packets from both AP and STA interfaces.
            // We need to make sure we only respond to requests on the AP interface.
            if ((from.sin_addr.s_addr & _ip_info_of_softap.netmask.addr) ==
                (_ip_info_of_softap.ip.addr & _ip_info_of_softap.netmask.addr)) {
                capt_dns_recv(&from, udp_msg, ret);
            } else {
                ESP_LOGI(TAG, "Ignoring packet from wrong interface.");
            }
        }
    }

    ESP_LOGI(TAG, "Closing captive portal DNS listen socket");
    close(_sockFd);

    xEventGroupClearBits(_capt_dns_event_group, DNS_SERVER_STOP_REQUESTED_EVENT);
    xEventGroupSetBits(_capt_dns_event_group, DNS_SERVER_STOP_COMPLETE_EVENT);

    vTaskDelete(NULL);
}

esp_err_t capt_dns_start(esp_netif_t* softap_netif_handle)
{
    esp_err_t ret = esp_netif_get_ip_info(softap_netif_handle, &_ip_info_of_softap);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP info for softAP interface.");
        return ret;
    }

    // xEventGroupCreateStatic() is not available?
    _capt_dns_event_group = xEventGroupCreate();

    ESP_LOGI(TAG, "Activating captive portal DNS server");

    // ESP-IDF UDP server example passes the address family as the pvParameters
    // argument to xTaskCreate.  In our case, we are always using AF_INET, so
    // we pass NULL for pvParameters instead.
    xTaskCreate(capt_dns_task, "captdns_task", 4096, NULL, 5, NULL);

    return ESP_OK;
}

void capt_dns_stop(void)
{
    if (!_capt_dns_event_group) {
        return;
    }

    ESP_LOGI(TAG, "Signaling DNS server task to close socket and shut down");

    // Signal the server thread to stop and then wait for it to signal back.
    xEventGroupSetBits(_capt_dns_event_group, DNS_SERVER_STOP_REQUESTED_EVENT);
    xEventGroupWaitBits(_capt_dns_event_group, DNS_SERVER_STOP_COMPLETE_EVENT, false, true,
                        portMAX_DELAY);

    vEventGroupDelete(_capt_dns_event_group);
    _capt_dns_event_group = NULL;

    ESP_LOGI(TAG, "Captive portal DNS server deactivated.");
}
