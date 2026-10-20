/* ABI 17 compatibility only. New clients use typed NETCTL and format in EL0.
 * The old private-profile reader remains here, outside driver and Wi-Fi core. */
#include "lume/net.h"
#include "lume/wireless.h"
#include "lume/vfs.h"
#include <string.h>
struct text {char *p;size_t cap,used;};
static void add(struct text *t,const char *s)
{while(*s && t->used+1<t->cap)t->p[t->used++]=*s++;t->p[t->used]=0;}
static void decimal(struct text *t,uint64_t n)
{char b[21];unsigned i=0;do{b[i++]=(char)('0'+n%10);n/=10;}while(n);while(i){char s[2]={b[--i],0};add(t,s);}}
static void address(struct text *t,const unsigned char ip[4])
{for(unsigned i=0;i<4;++i){if(i)add(t,".");decimal(t,ip[i]);}}
static void mac_text(struct text *t,const unsigned char *mac)
{const char *h="0123456789abcdef";for(unsigned i=0;i<6;++i){char b[4]={h[mac[i]>>4],h[mac[i]&15],i==5?0:':',0};add(t,b);}}
static uint32_t default_interface(void)
{
    uint32_t fallback=0;
    for(unsigned i=0;i<lume_netif_count();++i){struct lume_netif_info n={0};lume_netif_info(lume_netif_at(i),&n);if(n.kind==LUME_IF_WIFI)return n.ifindex;if(!fallback)fallback=n.ifindex;}
    return fallback;
}
static bool join_config(uint32_t index)
{
    struct lume_files f;lume_files_init(&f);unsigned char cfg[104]={0};struct lume_wifi_connect request={0};
    int64_t fd=lume_vfs_open(&f,"/data/system/wifi.cfg",LUME_O_RDONLY);struct lume_stat info;
    bool ok=fd>=0 && !lume_vfs_stat(&f,(uint64_t)fd,&info) && info.size==sizeof(cfg) &&
        lume_vfs_read(&f,(uint64_t)fd,cfg,sizeof(cfg))==(int64_t)sizeof(cfg);lume_files_release(&f);
    ok=ok && !memcmp(cfg,"LWN1",4) && cfg[4]>0 && cfg[4]<=32 && cfg[5]>=8 && cfg[5]<=63 && !cfg[6] && !cfg[7];
    if(ok){request.ssid_bytes=cfg[4];request.key_bytes=cfg[5];memcpy(request.ssid,cfg+8,request.ssid_bytes);memcpy(request.key,cfg+40,request.key_bytes);lume_net_release(index);ok=lume_wireless_connect(index,&request)==0;}
    volatile unsigned char *wipe=cfg;for(unsigned i=0;i<sizeof(cfg);++i)wipe[i]=0;
    wipe=(void *)&request;for(unsigned i=0;i<sizeof(request);++i)wipe[i]=0;return ok;
}
int64_t lume_net_command(unsigned op,const char *arg,char *out,size_t cap)
{
    if(!out || !cap || !arg)return -LUME_EINVAL;
    struct text t={out,cap,0};out[0]=0;uint32_t index=default_interface();
    if(!index){add(&t,"network unavailable\n");return -LUME_EIO;}
    lume_net_poll();struct lume_netif_info n={0};(void)lume_net_snapshot(index,&n);
    switch(op) {
    case LUME_NET_STATUS:
        add(&t,n.name);add(&t,"  ");mac_text(&t,n.mac);add(&t,(n.flags&LUME_IF_LINK)?"  associated\n":"  disconnected\n");
        add(&t,"inet ");address(&t,n.address);add(&t," mask ");address(&t,n.netmask);add(&t," gateway ");address(&t,n.gateway);
        add(&t,"\ndns ");address(&t,n.dns);add(&t,"\nRX ");decimal(&t,n.rx_packets);add(&t," TX ");decimal(&t,n.tx_packets);
        add(&t," RX-errors ");decimal(&t,n.rx_errors);add(&t," TX-errors ");decimal(&t,n.tx_errors);add(&t,"\n");break;
    case LUME_NET_SCAN: {
        struct lume_wifi_bss_info aps[LUME_NET_SCAN_MAX];int count=lume_wireless_scan(index,aps,LUME_NET_SCAN_MAX);if(count<0)return count;
        for(int i=0;i<count;++i){struct lume_wifi_bss_info *a=&aps[i];mac_text(&t,a->bssid);add(&t," ch ");decimal(&t,a->channel);
            add(&t," -");decimal(&t,a->rssi<0?(unsigned)-a->rssi:(unsigned)a->rssi);add(&t,"dBm ");
            add(&t,(a->security&LUME_BSS_RSN)?"RSN ":(a->security&LUME_BSS_WPA)?"WPA ":a->security?"privacy ":"open ");
            for(unsigned j=0;j<a->ssid_bytes;++j){char s[2]={a->ssid[j]>=32 && a->ssid[j]<127?(char)a->ssid[j]:'?',0};add(&t,s);}add(&t,"\n");}break;
    }
    case LUME_NET_JOIN:
        if(!join_config(index))return -LUME_EIO;
        lume_net_poll();add(&t,"WPA2 associated\n");if(!lume_net_dhcp(index))return -LUME_EAGAIN;
        (void)lume_net_snapshot(index,&n);add(&t,"DHCP ");address(&t,n.address);add(&t,"\n");break;
    case LUME_NET_DHCP:if(!lume_net_dhcp(index))return -LUME_EAGAIN;add(&t,"DHCP ready\n");break;
    case LUME_NET_LEAVE:lume_net_release(index);(void)lume_wireless_disconnect(index);lume_net_poll();add(&t,"disconnected\n");break;
    case LUME_NET_DNS:case LUME_NET_PING: {
        unsigned char ip[4];bool ok=op==LUME_NET_DNS?lume_net_resolve(arg,ip):lume_net_ping(arg,ip);
        if(!ok)return -LUME_EIO;address(&t,ip);add(&t,op==LUME_NET_PING?" ICMP reply\n":"\n");break;
    }
    case LUME_NET_TCP:if(!lume_net_tcp(arg))return -LUME_EIO;add(&t,"TCP echo byte-exact PASS\n");break;
    default:return -LUME_EINVAL;
    }
    return (int64_t)t.used;
}
