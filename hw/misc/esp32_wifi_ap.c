/* Vendored from github.com/lcgamboa/qemu @ b49c8e1 (MIT). */
/**
 * QEMU WLAN access point emulation
 *
 * Copyright (c) 2008 Clemens Kolbitsch
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Modifications:
 *  2008-February-24  Clemens Kolbitsch :
 *                                  New implementation based on ne2000.c
 *  18/01/22 Martin Johnson : Modified for esp32 wifi emulation
 *  08/12/23 lcgamboa@yahoo.com : Modified for esp32c3, espnow and SoftAp wifi emulation
 */


#include "qemu/osdep.h"
#include "net/net.h"
#include "qemu/log.h"
#include "qemu/timer.h"

#include "hw/irq.h"
#include "hw/misc/esp32_wifi.h"
#include "hw/misc/esp32_phya.h"
#include "esp32_wlan.h"
#include "esp32_wlan_packet.h"

// 50ms between beacons
#define BEACON_TIME 500000000
#define INTER_FRAME_TIME 5000000
#define WAIT_ACK_TIMEOUT 10000000
#define DEBUG 0
#define ENABLE_BEACON 1
#define DEBUG_DUMPFRAMES 0
#define DEBUG_WIRESHARK_IMPORT 0
#define ETHERTYPE_QEMU_RAW_80211 0x88f0

static const uint8_t softap_station_mac[6] = {
    0x10, 0x01, 0x00, 0xc4, 0x0a, 0x55
};

static const char *wlan_type_name(unsigned type)
{
    switch (type) {
    case IEEE80211_TYPE_MGT:
        return "MGT";
    case IEEE80211_TYPE_CTL:
        return "CTL";
    case IEEE80211_TYPE_DATA:
        return "DATA";
    default:
        return "UNKNOWN";
    }
}

static const char *wlan_mgt_subtype_name(unsigned subtype)
{
    switch (subtype) {
    case IEEE80211_TYPE_MGT_SUBTYPE_BEACON:
        return "BEACON";
    case IEEE80211_TYPE_MGT_SUBTYPE_ACTION:
        return "ACTION";
    case IEEE80211_TYPE_MGT_SUBTYPE_PROBE_REQ:
        return "PROBE_REQ";
    case IEEE80211_TYPE_MGT_SUBTYPE_PROBE_RESP:
        return "PROBE_RESP";
    case IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION:
        return "AUTH";
    case IEEE80211_TYPE_MGT_SUBTYPE_DEAUTHENTICATION:
        return "DEAUTH";
    case IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_REQ:
        return "ASSOC_REQ";
    case IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_RESP:
        return "ASSOC_RESP";
    case IEEE80211_TYPE_MGT_SUBTYPE_DISASSOCIATION:
        return "DISASSOC";
    default:
        return "MGT_OTHER";
    }
}

static const char *wlan_subtype_name(unsigned type, unsigned subtype)
{
    if (type == IEEE80211_TYPE_MGT) {
        return wlan_mgt_subtype_name(subtype);
    }
    if (type == IEEE80211_TYPE_CTL && subtype == IEEE80211_TYPE_CTL_SUBTYPE_ACK) {
        return "ACK";
    }
    if (type == IEEE80211_TYPE_DATA && subtype == IEEE80211_TYPE_DATA_SUBTYPE_DATA) {
        return "DATA";
    }
    return "OTHER";
}

static void wlan_log_frame(const char *dir, Esp32WifiState *s,
                           const struct mac80211_frame *frame)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "ESP32_WIFI_FRAME dir=%s type=%s subtype=%s channel=%d "
                  "mode=%u ap_state=%u flags=0x%x "
                  "addr1=%02x:%02x:%02x:%02x:%02x:%02x "
                  "addr2=%02x:%02x:%02x:%02x:%02x:%02x "
                  "addr3=%02x:%02x:%02x:%02x:%02x:%02x len=%u\n",
                  dir,
                  wlan_type_name(frame->frame_control.type),
                  wlan_subtype_name(frame->frame_control.type,
                                    frame->frame_control.sub_type),
                  esp32_wifi_channel,
                  s->mode,
                  s->ap_state,
                  frame->frame_control.flags,
                  frame->destination_address[0], frame->destination_address[1],
                  frame->destination_address[2], frame->destination_address[3],
                  frame->destination_address[4], frame->destination_address[5],
                  frame->source_address[0], frame->source_address[1],
                  frame->source_address[2], frame->source_address[3],
                  frame->source_address[4], frame->source_address[5],
                  frame->bssid_address[0], frame->bssid_address[1],
                  frame->bssid_address[2], frame->bssid_address[3],
                  frame->bssid_address[4], frame->bssid_address[5],
                  frame->frame_length);
}

static void wlan_log_eth(const char *dir, const uint8_t *buf, size_t size)
{
    if (size < 14) {
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "ESP32_WIFI_ETH dir=%s dst=%02x:%02x:%02x:%02x:%02x:%02x "
                  "src=%02x:%02x:%02x:%02x:%02x:%02x type=0x%02x%02x len=%zu\n",
                  dir,
                  buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
                  buf[6], buf[7], buf[8], buf[9], buf[10], buf[11],
                  buf[12], buf[13], size);
}

// color defines
#define BLACK 0
#define RED 1
#define GREEN 2
#define YELLOW 3
#define BLUE 4
#define MAGNETA 5
#define CYAN 6
#define WHITE 7
#define ANSI_DEFAULT() printf("\033[0m")
#define ANSI_COLOR(f, b) printf("\033[%d;%dm", (f) + 30, (b) + 40)
#define ANSI_FG_LCOLOR(f) printf("\033[0;%dm", (f) + 30)
#define ANSI_FG_HCOLOR(f) printf("\033[1;%dm", (f) + 30)

access_point_info access_points[]={
    /* QEMU-AP on channel 0 because esp32_wifi_channel is pinned to 0
     * (esp32_ana.c not vendored).  See esp32_wifi.c. */
    {"QEMU-AP",0,-20,{0x10,0x01,0x00,0xc4,0x0a,0x55}},
    {"PICSimLabWifi",1,-25,{0x10,0x01,0x00,0xc4,0x0a,0x56}},
    {"Espressif",5,-30,{0x10,0x01,0x00,0xc4,0x0a,0x51}},
    {"MasseyWifi",10,-40,{0x10,0x01,0x00,0xc4,0x0a,0x52}}
};

int nb_aps=sizeof(access_points)/sizeof(access_point_info);

static void Esp32_WLAN_beacon_timer(void *opaque)
{ 
    struct mac80211_frame *frame;
    Esp32WifiState *s = (Esp32WifiState *)opaque;
    // only send a beacon if we are an access point
    if((ENABLE_BEACON)&&(s->mode == Esp32_Mode_Station)){
      if(s->ap_state!=Esp32_WLAN__STATE_STA_ASSOCIATED) {
        for(int i=0;i<nb_aps;i++){
          int ap = (i + s->beacon_ap)%nb_aps;
          if (access_points[ap].channel==esp32_wifi_channel) {
              memcpy(s->ap_macaddr,access_points[ap].mac_address,6);
              frame = Esp32_WLAN_create_beacon_frame(&access_points[ap]);
              Esp32_WLAN_init_ap_frame(s, frame);
              Esp32_WLAN_insert_frame(s, frame);
              break;
          }
        }
        s->beacon_ap=(s->beacon_ap+1)%nb_aps;
      } 
    }
    timer_mod(s->beacon_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + BEACON_TIME);
}

static void Esp32_WLAN_inject_timer(void *opaque)
{
    Esp32WifiState *s = (Esp32WifiState *)opaque;
    struct mac80211_frame *frame;

    frame = s->inject_queue;
    if (frame) {
        // remove from queue
        s->inject_queue_size--;
        s->inject_queue = frame->next_frame;
        Esp32_sendFrame(s, (void *)frame, frame->frame_length,frame->signal_strength);
        free(frame);
    }
    if (s->inject_queue_size > 0) {
        // there are more packets... schedule
        // the timer for sending them as well
        timer_mod(s->inject_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + INTER_FRAME_TIME);
    } else {
        // we wait until a new packet schedules
        // us again
        s->inject_timer_running = 0;
    }

}

static void Esp32_WLAN_Wait_ACk_timer(void *opaque)
{
    Esp32WifiState *s = (Esp32WifiState *)opaque;

    Esp32_WLAN_frame_delivered(s);
}

static void macprint(const uint8_t *p, const char * name) {
    printf("%s: %02x:%02x:%02x:%02x:%02x:%02x\n",name, p[0],p[1],p[2],p[3],p[4],p[5]);
}

static void buffer_print(uint8_t * b, unsigned size){
    for(int i=0;i < size ;i++) {
        if((i%16)==0) printf("%04x: ",i);
        printf("%02x ",b[i]);
        if((i%16)==15) printf("\n");
    }
    if((size%16)!=0)printf("\n");
}

static void infoprint(struct mac80211_frame *frame) {
    if(DEBUG_DUMPFRAMES) {
#if  (DEBUG_WIRESHARK_IMPORT == 0 )
        printf("\nFrame Info type:%d subtype:%d flags:%d duration:%d length:%d\n",frame->frame_control.type,
           frame->frame_control.sub_type,frame->frame_control.flags,frame->duration_id, frame->frame_length);

        printf("protocol_version: %i\n",frame->frame_control.protocol_version);    
        printf("type: %i ",frame->frame_control.type );
        switch (frame->frame_control.type)
        {
        case IEEE80211_TYPE_MGT:
            printf("MGT\n"); 
            break;
        case IEEE80211_TYPE_CTL:
            printf("CTL\n"); 
            break;
       case IEEE80211_TYPE_DATA:
            printf("DATA\n"); 
            break;
        }    
        printf("subtype: %i  ",frame->frame_control.sub_type);     
        switch (frame->frame_control.type)
        {
        case IEEE80211_TYPE_MGT:
            switch (frame->frame_control.sub_type)
            {
            case IEEE80211_TYPE_MGT_SUBTYPE_BEACON:
                printf("BEACON\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_ACTION:
                printf("ACTION\n");
                break;                      
            case IEEE80211_TYPE_MGT_SUBTYPE_PROBE_REQ:
                printf("PROBE_REQ\n");
                break;       
            case IEEE80211_TYPE_MGT_SUBTYPE_PROBE_RESP:  
                printf("PROBE_RESP\n");
                break;     
            case IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION:
                printf("AUTHENTICATION\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_DEAUTHENTICATION:
                printf("DEAUTHENTICATION\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_REQ:
                printf("ASSOCIATION_REQ\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_RESP:
                printf("ASSOCIATION_RESP\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_DISASSOCIATION:
                printf("DISASSOCIATION\n");
                break;
            default:
                printf("---\n");
                break;
            }
            break;
        case IEEE80211_TYPE_CTL:
           switch (frame->frame_control.sub_type)
            {
            case IEEE80211_TYPE_CTL_SUBTYPE_ACK:
                printf("ACK\n");
                break;
            default:
                printf("---\n");
                break;
            }
            break;
       case IEEE80211_TYPE_DATA:
            switch (frame->frame_control.sub_type)
            {
            case IEEE80211_TYPE_DATA_SUBTYPE_DATA:
                printf("DATA\n");
                break;
            default:
                printf("---\n");
                break;
            }           
            break;
        }

        printf("flags: %i\n",frame->frame_control.flags);
        printf("duration_id: %i\n",frame->duration_id);
        macprint(frame->destination_address,"destination");
        macprint(frame->source_address,"source");
        macprint(frame->bssid_address,"bssid");
        printf("fragment_number: %u\n",frame->sequence_control.fragment_number);     
        printf("sequence_number : %u\n",frame->sequence_control.sequence_number);         
        
        switch (frame->frame_control.type)
        {
          case IEEE80211_TYPE_MGT:
            switch (frame->frame_control.sub_type)
            {        
              case IEEE80211_TYPE_MGT_SUBTYPE_BEACON:
                printf("beacon timestamp : %lu\n",(unsigned long) frame->beacon_info.timestamp);
                printf("beacon interval : %u\n",frame->beacon_info.interval);
                printf("beacon capability : %u\n",frame->beacon_info.capability);
                break;  
              default:
                if(frame->frame_length  > IEEE80211_HEADER_SIZE)
                   buffer_print((uint8_t *)frame->data_and_fcs, frame->frame_length - IEEE80211_HEADER_SIZE);
                break;
            }
            break; 
          case IEEE80211_TYPE_CTL:
            switch (frame->frame_control.sub_type)
            {        
              default:
                if(frame->frame_length  > IEEE80211_HEADER_SIZE)
                   buffer_print((uint8_t *)frame->data_and_fcs, frame->frame_length - IEEE80211_HEADER_SIZE);
                break;
            }
            break; 
            case IEEE80211_TYPE_DATA:
            switch (frame->frame_control.sub_type)
            {        
              default:
                printf("EtherType 0x%02X%02X\n",frame->data_and_fcs[6],frame->data_and_fcs[7]);
                if( frame->data_and_fcs[6]==8 && frame->data_and_fcs[7]==0x06) {
                    printf("ARP\n");
                    arp_header_t *header=(arp_header_t *)&frame->data_and_fcs[8];
                    printf("Operation %i\n", header->operation>>8);
                }
                if( frame->data_and_fcs[6]==8 && frame->data_and_fcs[7]==0x00) {
                    printf("IPV4\n");
                    ip_header_t *header=(ip_header_t *)&frame->data_and_fcs[8];
                    printf("Protocol: 0x%04X\n",header->protocol);
                    switch (header->protocol)
                    {
                    case 0x11:
                        printf("UDP\n");
                        break;
                    case 0x06:
                        printf("TCP\n");
                        break;
                    }
                }
                if( frame->data_and_fcs[6]==8 && frame->data_and_fcs[7]==0xdd) {
                    printf("IPV6\n");
                }
                if(frame->frame_length  > IEEE80211_HEADER_SIZE)
                   buffer_print((uint8_t *)frame->data_and_fcs, frame->frame_length - IEEE80211_HEADER_SIZE);
                break;
            }
            break;   
        }
        printf("frame_length : %u\n", frame->frame_length);
#else       
        uint8_t *b=(uint8_t *)frame;
        for(int i=0;i<frame->frame_length;i++) {
            if((i%16)==0) printf("\n%04x: ",i);
            printf("%02x ",b[i]);
        }
        printf("\n");
#endif        
    }
}

void Esp32_WLAN_insert_frame(Esp32WifiState *s, struct mac80211_frame *frame)
{
    struct mac80211_frame *i_frame;

    insertCRC(frame);
    if(DEBUG) {
        ANSI_FG_HCOLOR(GREEN);
        printf("---------------\n>IN Send Frame type:%d subtype:%d channel:%d ap_state:%d  time:%ld \n",frame->frame_control.type,frame->frame_control.sub_type,esp32_wifi_channel,s->ap_state,(unsigned long) qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        ANSI_DEFAULT();
    }
    infoprint(frame);
    s->inject_queue_size++;
    i_frame = s->inject_queue;
    if (!i_frame) {
        s->inject_queue = frame;
    } else {
        while (i_frame->next_frame) {
            i_frame = i_frame->next_frame;
        }
        i_frame->next_frame = frame;
    }

    if (!s->inject_timer_running) {
        // if the injection timer is not
        // running currently, let's schedule
        // one run...
        s->inject_timer_running = 1;
        timer_mod(s->inject_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + INTER_FRAME_TIME);
    }

}

static _Bool Esp32_WLAN_can_receive(NetClientState *ncs)
{
    Esp32WifiState *s = qemu_get_nic_opaque(ncs);
    /*
    if (s->ap_state != Esp32_WLAN__STATE_ASSOCIATED  && s->ap_state != Esp32_WLAN__STATE_STA_ASSOCIATED) {
        // we are currently not connected
        // to the access point
        return 0;
    }
    */
    if (s->inject_queue_size > Esp32_WLAN__MAX_INJECT_QUEUE_SIZE) {
        // overload, please give me some time...
        return 0;
    }

    return 1;
}

static ssize_t Esp32_WLAN_receive(NetClientState *ncs,
                                    const uint8_t *buf, size_t size)
{
    Esp32WifiState *s = qemu_get_nic_opaque(ncs);
    struct mac80211_frame *frame;
    if (!Esp32_WLAN_can_receive(ncs)) {
        // this should not happen, but in
        // case it does, let's simply drop
        // the packet
        return -1;
    }

    if (!s) {
        return -1;
    }
    /*
     * A 802.3 packet comes from the qemu network. The
     * access points turns it into a 802.11 frame and
     * forwards it to the wireless device
     */
    if (size >= 38 &&
        buf[12] == ((ETHERTYPE_QEMU_RAW_80211 >> 8) & 0xff) &&
        buf[13] == (ETHERTYPE_QEMU_RAW_80211 & 0xff)) {
        frame = g_malloc0(sizeof(*frame));
        size_t raw_size = size - 14;
        if (raw_size > IEEE80211_HEADER_SIZE + sizeof(frame->data_and_fcs)) {
            raw_size = sizeof(frame->data_and_fcs) + IEEE80211_HEADER_SIZE;
        }
        memcpy(frame, buf + 14, raw_size);
        frame->frame_length = raw_size;
        frame->signal_strength = -30;
        frame->next_frame = NULL;
        wlan_log_frame("raw80211-to-guest", s, frame);
        Esp32_WLAN_insert_frame(s, frame);
        return size;
    }

    frame = Esp32_WLAN_create_data_packet(s, buf, size);
    if (frame) {
        wlan_log_eth("netdev-to-guest", buf, size);
        if (size >= 34 && buf[12] == 0x08 && buf[13] == 0x00) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ESP32_WIFI_IPV4 dir=netdev-to-guest proto=%u "
                          "src=%u.%u.%u.%u dst=%u.%u.%u.%u len=%zu\n",
                          buf[23],
                          buf[26], buf[27], buf[28], buf[29],
                          buf[30], buf[31], buf[32], buf[33],
                          size);
        }
        if (size >= 42 && buf[12] == 0x08 && buf[13] == 0x06) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ESP32_WIFI_ARP dir=netdev-to-guest op=%u target=%u.%u.%u.%u len=%zu\n",
                          ((unsigned)buf[20] << 8) | buf[21],
                          buf[38], buf[39], buf[40], buf[41],
                          size);
        }
        if(s->mode == Esp32_Mode_Station){
             memcpy(s->ap_macaddr,s->associated_ap_macaddr,6);
        
            if(s->ap_state==Esp32_WLAN__STATE_STA_ASSOCIATED) {
                frame->frame_control.flags=1;
                // if it's an arp request put the correct reply mac address in the packet 
                if( frame->data_and_fcs[6]==8 && frame->data_and_fcs[7]==6) {
                    memcpy(frame->data_and_fcs+16,s->macaddr,6);
                }
            }
        }
        else{
            memcpy(frame->destination_address, s->softap_macaddr,6);
            if(s->ap_state==Esp32_WLAN__STATE_STA_ASSOCIATED) {
                frame->frame_control.flags=1;
            }
        }


        if(((buf[12] & 0xE0) == IEEE80211_ENCAPSULATED)&&(buf[13] == ((IEEE80211_TYPE_MGT << 4)|IEEE80211_TYPE_MGT_SUBTYPE_ACTION)))
        {
          unsigned long ethernet_frame_size;
          unsigned char ethernet_frame[1518];
          struct mac80211_frame *reply = NULL;

          //discard packages from others channels
          if((buf[12] & 0x0F) != esp32_wifi_channel) return -1;
          //discard packages originated from the same mac address
          if(!memcmp(&buf[6],s->macaddr,6)) return -1;
          //check destination
          if((memcmp(&buf[0],BROADCAST,6))&&(memcmp(&buf[0],s->macaddr,6))) return -1;

          //action frame  
          Esp32_WLAN_init_ap_frame(s, frame);
          memcpy(frame->data_and_fcs, buf+14, size-14);   
          memcpy(frame->destination_address, &buf[0], 6);
          memcpy(frame->source_address, &buf[6], 6);
          memset(frame->bssid_address, 0xff, 6);
          frame->frame_control.type= IEEE80211_TYPE_MGT;
          frame->frame_control.sub_type= IEEE80211_TYPE_MGT_SUBTYPE_ACTION;
          frame->frame_control.flags=  ((buf[12] & 0xF0) == IEEE80211_ENCAPSULATED_PROTECTED) ? 0x40 : 00;
          frame->duration_id= 0 ;
          frame->frame_length-=12;
          Esp32_WLAN_insert_frame(s, frame);


           //if destination is broadcast, don´t send ack 
          if(!memcmp(&buf[0],BROADCAST,6)) return size;

          //Send Ack
          reply = Esp32_WLAN_create_ack();
          memcpy(reply->destination_address, &buf[6], 6);
          memcpy(reply->source_address, s->macaddr, 6);
          memset(reply->bssid_address, 0xff, 6);
          reply->duration_id=0;

          if(DEBUG) {
            ANSI_FG_HCOLOR(RED);
            macprint(reply->destination_address,"---------------\n<OUT ACK send: dest ");
            ANSI_DEFAULT();
          }
          infoprint(reply);
           /*
            * The access point uses the 802.11 frame
            * and sends a 802.3 frame into the network...
            * This packet is then understandable by
            * qemu-slirp
            *
            * If we ever want the access point to offer
            * some services, it can be added here!!
            */
          // ethernet header type
          ethernet_frame[12] = IEEE80211_ENCAPSULATED | esp32_wifi_channel;
          ethernet_frame[13] = ((IEEE80211_TYPE_CTL << 4)|IEEE80211_TYPE_CTL_SUBTYPE_ACK); 

          memcpy(&ethernet_frame[0], reply->destination_address, 6);
          memcpy(&ethernet_frame[6], reply->source_address, 6);

          // add size of ethernet header
          ethernet_frame_size = 14;
          /*
           * Send 802.3 frame
           */
          qemu_send_packet(qemu_get_queue(s->nic), ethernet_frame, ethernet_frame_size);

          return size; 
        }

        if(((buf[12] & 0xF0)== IEEE80211_ENCAPSULATED)&&(buf[13] == ((IEEE80211_TYPE_CTL << 4)|IEEE80211_TYPE_CTL_SUBTYPE_ACK)))
        {

          //discard packages from others channels
          if((buf[12] & 0x0F) != esp32_wifi_channel) return -1;  
          //discard packages originated from the same mac address
          if(!memcmp(&buf[6],s->macaddr,6)) return -1;
          //check destination
          if(memcmp(&buf[0],s->macaddr,6)) return -1;

          Esp32_WLAN_Set_Packet_Status(ESP32_PHYA_ACK);
          timer_mod_anticipate(s->wait_ack_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));

          Esp32_WLAN_init_ap_frame(s, frame); 
          memcpy(frame->destination_address, &buf[0], 6);
          memcpy(frame->source_address, &buf[6], 6);
          memset(frame->bssid_address, 0xff, 6); 
          frame->frame_control.type= IEEE80211_TYPE_CTL;
          frame->frame_control.sub_type= IEEE80211_TYPE_CTL_SUBTYPE_ACK;
          frame->frame_control.flags= 0; 
          frame->duration_id= 0 ;
          frame->frame_length = 10;

          if(DEBUG) {
            ANSI_FG_HCOLOR(GREEN);
            macprint(&buf[6],"---------------\n>IN ACK received: source");
            ANSI_DEFAULT();
          }
          infoprint(frame);
          //ACK is not inserted in DMA
          return size; 
        }
        Esp32_WLAN_init_ap_frame(s, frame);
        if (s->mode == Esp32_Mode_SoftAP) {
            memcpy(frame->destination_address, s->softap_macaddr, 6);
            memcpy(frame->source_address, &buf[6], 6);
            memcpy(frame->bssid_address, &buf[0], 6);
            if (s->softap_privacy && size >= 14 &&
                !(buf[12] == 0x88 && buf[13] == 0x8e)) {
                frame->frame_control.flags |= 0x40;
            }
        }
        wlan_log_frame("netdev-to-guest", s, frame);
        Esp32_WLAN_insert_frame(s, frame);
    }
    return size;
}
static void Esp32_WLAN_cleanup(NetClientState *ncs) { }

static NetClientInfo net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = Esp32_WLAN_can_receive,
    .receive = Esp32_WLAN_receive,
    .cleanup = Esp32_WLAN_cleanup,
};

void Esp32_WLAN_reset_ap(Esp32WifiState *s) {
    esp32_wifi_channel = 0;
    s->ap_state = Esp32_WLAN__STATE_NOT_AUTHENTICATED;
    s->beacon_ap=0;
    s->mode = Esp32_Mode_Station;
    s->softap_privacy = 0;
    memcpy(s->ap_macaddr,(uint8_t[]){0x10,0x13,0x46,0xbf,0x31,0x50},sizeof(s->ap_macaddr));

    s->inject_timer_running = 0;
    s->inject_sequence_number = 0;

    s->inject_queue = NULL;
    s->inject_queue_size = 0;
}

void Esp32_WLAN_setup_ap(DeviceState *dev,Esp32WifiState *s) {

    Esp32_WLAN_reset_ap(s);

    s->beacon_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, Esp32_WLAN_beacon_timer, s);
    timer_mod(s->beacon_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)+BEACON_TIME);

    // setup the timer but only schedule
    // it when necessary...
    s->inject_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, Esp32_WLAN_inject_timer, s);

    s->wait_ack_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, Esp32_WLAN_Wait_ACk_timer, s);

    s->nic = qemu_new_nic(&net_info, &s->conf, object_get_typename(OBJECT(s)), dev->id, &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->macaddr);
}

static void send_single_frame(Esp32WifiState *s, struct mac80211_frame *frame, struct mac80211_frame *reply) {
    reply->sequence_control.sequence_number = s->inject_sequence_number++ +0x730;
    reply->signal_strength=-10;

    if(frame) {
        memcpy(reply->destination_address, frame->source_address, 6);
        if(s->mode == Esp32_Mode_Station){
           memcpy(reply->source_address, s->macaddr, 6);
        }
        else{
           memcpy(reply->source_address, s->ap_macaddr, 6);      
        }
        memcpy(reply->bssid_address, frame->source_address, 6);
    }
    
    Esp32_WLAN_insert_frame(s, reply);
}

void Esp32_WLAN_handle_frame(Esp32WifiState *s, struct mac80211_frame *frame)
{
    struct mac80211_frame *reply = NULL;
    static access_point_info dummy_ap={0};
    static char ssid[64];
    static bool softap_requires_rsn;
    unsigned long ethernet_frame_size;
    unsigned char ethernet_frame[1518];

    
    if(DEBUG){ 
        if((ENABLE_BEACON)||!((frame->frame_control.type == IEEE80211_TYPE_MGT)&&(frame->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_BEACON))){
            ANSI_FG_HCOLOR(RED);
            printf("-------------------------\n<OUT Handle Frame type:%d subtype:%d channel:%d ap_state:%d time:%ld \n",frame->frame_control.type,
                         frame->frame_control.sub_type,esp32_wifi_channel,s->ap_state,(unsigned long)  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            ANSI_DEFAULT();
        }
    }
    if((ENABLE_BEACON)||!((frame->frame_control.type == IEEE80211_TYPE_MGT)&&(frame->frame_control.sub_type == IEEE80211_TYPE_MGT_SUBTYPE_BEACON))){
        infoprint(frame);
    }
    wlan_log_frame("guest-to-wlan", s, frame);
    access_point_info *ap_info=0;

    if(s->mode == Esp32_Mode_Station){
        for(int i=0;i<nb_aps;i++)
            if(access_points[i].channel==esp32_wifi_channel){
                ap_info=&access_points[i];
                break;
        }
    }

    Esp32_WLAN_Set_Packet_Status(ESP32_PHYA_ACK);
    if(frame->frame_control.type == IEEE80211_TYPE_MGT) {        
        switch(frame->frame_control.sub_type) {
            case IEEE80211_TYPE_MGT_SUBTYPE_BEACON:
                if(s->ap_state==Esp32_WLAN__STATE_NOT_AUTHENTICATED ||
                   s->ap_state==Esp32_WLAN__STATE_AUTHENTICATED ||
                   s->ap_state==Esp32_WLAN__STATE_STA_NOT_AUTHENTICATED) {
                    strncpy(ssid,(char *)frame->data_and_fcs+14,frame->data_and_fcs[13]);
                    if(DEBUG) printf("beacon from %s\n",ssid);
                    dummy_ap.ssid=ssid;
                    dummy_ap.channel = esp32_wifi_channel;
                    dummy_ap.sigstrength= -23;
                    softap_requires_rsn = (frame->beacon_info.capability & 0x10) != 0;
                    s->softap_privacy = softap_requires_rsn ? 1 : 0;
                    memcpy(s->ap_macaddr, softap_station_mac,
                           sizeof(s->ap_macaddr));
                    memcpy(dummy_ap.mac_address,s->ap_macaddr,6) ;
                    memcpy(s->softap_macaddr,frame->bssid_address,6);
                    s->mode = Esp32_Mode_SoftAP;
                    s->ap_state=Esp32_WLAN__STATE_STA_NOT_AUTHENTICATED;
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "ESP32_WIFI_STATE mode=SoftAP ap_state=%u ssid=%s\n",
                                  s->ap_state, ssid);
                    send_single_frame(s,frame,Esp32_WLAN_create_probe_request(&dummy_ap));
                }
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_ACTION:    
                if(DEBUG) printf("action\n");
               /*
                * The access point uses the 802.11 frame
                * and sends a 802.3 frame into the network...
                * This packet is then understandable by
                * qemu-slirp
                *
                * If we ever want the access point to offer
                * some services, it can be added here!!
                */
                // ethernet header type
                ethernet_frame[12] = ((frame->frame_control.flags & 0x40) ? IEEE80211_ENCAPSULATED_PROTECTED : IEEE80211_ENCAPSULATED)| esp32_wifi_channel;
                ethernet_frame[13] = ((IEEE80211_TYPE_MGT << 4)|IEEE80211_TYPE_MGT_SUBTYPE_ACTION); 

                memcpy(&ethernet_frame[0], frame->destination_address, 6);
                memcpy(&ethernet_frame[6], s->macaddr, 6);

                // add packet content
                ethernet_frame_size = frame->frame_length -IEEE80211_HEADER_SIZE;

                if (ethernet_frame_size > sizeof(ethernet_frame)) {
                    ethernet_frame_size = sizeof(ethernet_frame);
                }
                memcpy(&ethernet_frame[14], &frame->data_and_fcs[0], ethernet_frame_size);
                // add size of ethernet header
                ethernet_frame_size += 14;
                /*
                * Send 802.3 frame
                */
                qemu_send_packet(qemu_get_queue(s->nic), ethernet_frame, ethernet_frame_size);
                 
                //if destination is not broadcast wait for ack 
                if(memcmp(&ethernet_frame[0],BROADCAST,6)){ 
                   Esp32_WLAN_Set_Packet_Status(ESP32_PHYA_NACK);
                   timer_mod(s->wait_ack_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + WAIT_ACK_TIMEOUT);
                   return;
                }
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_PROBE_RESP:
                ap_info=&dummy_ap;
                strncpy(ssid,(char *)frame->data_and_fcs+14,frame->data_and_fcs[13]);
                if(DEBUG) printf("probe resp from %s\n",ssid);
                dummy_ap.ssid=ssid;
                softap_requires_rsn = (frame->beacon_info.capability & 0x10) != 0;
                s->softap_privacy = softap_requires_rsn ? 1 : 0;
                s->ap_state=Esp32_WLAN__STATE_STA_NOT_AUTHENTICATED;
                send_single_frame(s,frame,Esp32_WLAN_create_deauthentication());
                send_single_frame(s,frame,Esp32_WLAN_create_authentication_request());
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_RESP:
                if(DEBUG) printf("assoc resp\n");
                if (s->mode == Esp32_Mode_SoftAP) {
                    memcpy(s->associated_ap_macaddr, s->macaddr,
                           sizeof(s->associated_ap_macaddr));
                    s->ap_state = Esp32_WLAN__STATE_STA_ASSOCIATED;
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "ESP32_WIFI_STATE mode=SoftAP ap_state=STA_ASSOCIATED "
                                  "client_ip=10.0.0.2 dhcp=skipped\n");
                    break;
                }
                mac80211_frame *frame1=Esp32_WLAN_create_dhcp_discover(s);
                memcpy(frame1->bssid_address,BROADCAST,6);
                memcpy(frame1->source_address,frame->destination_address,6);
                memcpy(frame1->destination_address,frame->source_address,6);
                send_single_frame(s,0,frame1);
                s->ap_state=Esp32_WLAN__STATE_STA_DHCP;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "ESP32_WIFI_STATE mode=SoftAP ap_state=STA_DHCP\n");
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_DISASSOCIATION:
                DEBUG_PRINT_AP(("Received disassociation!\n"));
                send_single_frame(s,frame,Esp32_WLAN_create_disassociation());
                if (s->mode == Esp32_Mode_SoftAP) {
                    s->ap_state = Esp32_WLAN__STATE_STA_NOT_AUTHENTICATED;
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "ESP32_WIFI_STATE mode=SoftAP ap_state=STA_NOT_AUTHENTICATED reason=disassoc\n");
                }
                if (s->ap_state == Esp32_WLAN__STATE_ASSOCIATED || s->ap_state == Esp32_WLAN__STATE_STA_ASSOCIATED) {
                    s->ap_state = Esp32_WLAN__STATE_AUTHENTICATED;
                }
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_DEAUTHENTICATION:
                DEBUG_PRINT_AP(("Received deauthentication!\n"));
                //reply = Esp32_WLAN_create_authentication_response(ap_info);
                if (s->mode == Esp32_Mode_SoftAP) {
                    s->ap_state = Esp32_WLAN__STATE_STA_NOT_AUTHENTICATED;
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "ESP32_WIFI_STATE mode=SoftAP ap_state=STA_NOT_AUTHENTICATED reason=deauth\n");
                }
                if (s->ap_state == Esp32_WLAN__STATE_AUTHENTICATED) {
                    s->ap_state = Esp32_WLAN__STATE_NOT_AUTHENTICATED;
                }
                break;
            case IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION:
                DEBUG_PRINT_AP(("Received authentication!\n"));
                if(frame->data_and_fcs[2]==2) { // response
                    if (softap_requires_rsn) {
                        send_single_frame(s, frame,
                            Esp32_WLAN_create_association_request_wpa2_psk(&dummy_ap));
                    } else {
                        send_single_frame(s, frame,
                            Esp32_WLAN_create_association_request(&dummy_ap));
                    }
                }
                break;
        }
        if(ap_info) {
            memcpy(s->ap_macaddr,ap_info->mac_address,6);
            switch(frame->frame_control.sub_type) {
                case IEEE80211_TYPE_MGT_SUBTYPE_PROBE_REQ:
                    DEBUG_PRINT_AP(("Received probe request!\n"));
                    reply = Esp32_WLAN_create_probe_response(ap_info);
                    break;
                case IEEE80211_TYPE_MGT_SUBTYPE_AUTHENTICATION:
                    DEBUG_PRINT_AP(("Received authentication req!\n"));
                    if(frame->data_and_fcs[2]==1) { // request
                        reply = Esp32_WLAN_create_authentication_response(ap_info);
                        if (s->ap_state == Esp32_WLAN__STATE_NOT_AUTHENTICATED) {
                            s->ap_state = Esp32_WLAN__STATE_AUTHENTICATED;
                        }
                    } 
                break;
                case IEEE80211_TYPE_MGT_SUBTYPE_ASSOCIATION_REQ:
                    DEBUG_PRINT_AP(("Received association request!\n"));
                    reply = Esp32_WLAN_create_association_response(ap_info);
                    if (s->ap_state == Esp32_WLAN__STATE_AUTHENTICATED) {
                        s->ap_state = Esp32_WLAN__STATE_ASSOCIATED;
                        memcpy(s->associated_ap_macaddr,s->ap_macaddr,6);
                    }
                    break;
            }
            if (reply) {
                reply->signal_strength=ap_info->sigstrength;
                memcpy(reply->destination_address, frame->source_address, 6);
                Esp32_WLAN_init_ap_frame(s, reply);
                Esp32_WLAN_insert_frame(s, reply);
            }
        }
    }
    if ((frame->frame_control.type == IEEE80211_TYPE_DATA) &&
        (frame->frame_control.sub_type == IEEE80211_TYPE_DATA_SUBTYPE_DATA)) {
        if(s->ap_state == Esp32_WLAN__STATE_STA_DHCP) {
            dhcp_request_t *req=(dhcp_request_t *)&frame->data_and_fcs[8];
            // check for a dhcp offer
            if(req->dhcp.bp_options[0]==0x35 && req->dhcp.bp_options[2]==0x2) {
                mac80211_frame *frame1=Esp32_WLAN_create_dhcp_request(s,req->dhcp.yiaddr);
                memcpy(frame1->bssid_address,BROADCAST,6);
                memcpy(frame1->source_address,s->ap_macaddr,6);
                memcpy(frame1->destination_address,frame->source_address,6);
                send_single_frame(s,0,frame1);
                //memcpy(s->ap_macaddr,(uint8_t[]){0x10,0x01,0x00,0xc4,0x0a,0x25},sizeof(s->ap_macaddr));
                //memcpy(s->associated_ap_macaddr,s->ap_macaddr,sizeof(s->ap_macaddr));
                memcpy(s->associated_ap_macaddr,s->macaddr,sizeof(s->macaddr));
                s->ap_state=Esp32_WLAN__STATE_STA_ASSOCIATED; 
                qemu_log_mask(LOG_GUEST_ERROR,
                              "ESP32_WIFI_STATE mode=SoftAP ap_state=STA_ASSOCIATED "
                              "client_ip=%u.%u.%u.%u\n",
                              req->dhcp.yiaddr[0], req->dhcp.yiaddr[1],
                              req->dhcp.yiaddr[2], req->dhcp.yiaddr[3]);
                if(DEBUG) printf("Dhcp received addr %d.%d.%d.%d\n",req->dhcp.yiaddr[0],req->dhcp.yiaddr[1],req->dhcp.yiaddr[2],req->dhcp.yiaddr[3]);
            }
        } else if (s->ap_state == Esp32_WLAN__STATE_ASSOCIATED || s->ap_state == Esp32_WLAN__STATE_STA_ASSOCIATED) {
            if (s->mode == Esp32_Mode_SoftAP &&
                (frame->frame_control.flags & 0x40)) {
                ethernet_frame[12] = (ETHERTYPE_QEMU_RAW_80211 >> 8) & 0xff;
                ethernet_frame[13] = ETHERTYPE_QEMU_RAW_80211 & 0xff;
                memcpy(&ethernet_frame[0], frame->destination_address, 6);
                memcpy(&ethernet_frame[6], frame->source_address, 6);
                ethernet_frame_size = frame->frame_length;
                if (ethernet_frame_size > sizeof(ethernet_frame) - 14) {
                    ethernet_frame_size = sizeof(ethernet_frame) - 14;
                }
                memcpy(&ethernet_frame[14], frame, ethernet_frame_size);
                ethernet_frame_size += 14;
                qemu_send_packet(qemu_get_queue(s->nic), ethernet_frame, ethernet_frame_size);
                wlan_log_eth("guest-to-netdev-raw80211", ethernet_frame, ethernet_frame_size);
                goto delivered;
            }
            /*
            * The access point uses the 802.11 frame
            * and sends a 802.3 frame into the network...
            * This packet is then understandable by
            * qemu-slirp
            *
            * If we ever want the access point to offer
            * some services, it can be added here!!
            */
            // ethernet header type
            ethernet_frame[12] = frame->data_and_fcs[6];
            ethernet_frame[13] = frame->data_and_fcs[7];

            // the new originator of the packet is
            // the access point
            if (s->mode == Esp32_Mode_SoftAP) {
                memcpy(&ethernet_frame[6], frame->source_address, 6);
            }
            else if(s->ap_state == Esp32_WLAN__STATE_ASSOCIATED)
                memcpy(&ethernet_frame[6], s->ap_macaddr, 6);
            else
                memcpy(&ethernet_frame[6], s->macaddr, 6);

            if (ethernet_frame[12] == 0x08 && ethernet_frame[13] == 0x06) {
                // for arp request, we use a broadcast
                memset(&ethernet_frame[0], 0xff, 6);
            } else {
                // otherwise we forward the packet to
                // where it really belongs
                memcpy(&ethernet_frame[0], frame->destination_address, 6);
            }

            // add packet content
            ethernet_frame_size = frame->frame_length - IEEE80211_HEADER_SIZE - 4 - 8;

            // for some reason, the packet is 22 bytes too small (??)
            ethernet_frame_size += 22;
            if (ethernet_frame_size > sizeof(ethernet_frame)) {
                ethernet_frame_size = sizeof(ethernet_frame);
            }
            memcpy(&ethernet_frame[14], &frame->data_and_fcs[8], ethernet_frame_size);
            // add size of ethernet header
            ethernet_frame_size += 14;
            /*
            * Send 802.3 frame
            */
            qemu_send_packet(qemu_get_queue(s->nic), ethernet_frame, ethernet_frame_size);
            wlan_log_eth("guest-to-netdev", ethernet_frame, ethernet_frame_size);
        }
    }
delivered:
    Esp32_WLAN_frame_delivered(s);
}
