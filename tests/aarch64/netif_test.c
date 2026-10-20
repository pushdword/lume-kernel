/* Actual registry, common Wi-Fi and structured dispatcher with two independent
 * fake backends. No CYW or lwIP implementation is substituted into the core. */
#include "lume/net.h"
#include "lume/wireless.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks,polls,releases,dhcps;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL netif line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
struct fake {bool link,fail_tx,fail_admin,fail_connect;int receive,scan_mode;unsigned sent,received,scans,connects,disconnects;uint32_t index;};
static void poll(void *ctx){(void)ctx;++polls;}
static bool link(void *ctx){return ((struct fake *)ctx)->link;}
static int receive(void *ctx,void *out,size_t cap)
{struct fake *f=ctx;++f->received;int n=f->receive;f->receive=0;if(n>0 && (size_t)n<=cap)memset(out,0,(size_t)n);return n;}
static bool transmit(void *ctx,const void *p,size_t n)
{struct fake *f=ctx;CHECK(p && n>=14);++f->sent;return !f->fail_tx;}
static bool admin(void *ctx,bool up)
{struct fake *f=ctx;if(f->fail_admin)return false;if(!up)f->link=false;return true;}
static const struct lume_netif_ops data_ops={poll,link,receive,transmit,admin};
static int scan(void *ctx,struct lume_wifi_bss_info *out,unsigned cap)
{
    struct fake *f=ctx;++f->scans;CHECK(cap>0 && cap<=32);
    if(f->scan_mode==1)return -1;if(f->scan_mode==2)return (int)cap+1;
    memset(out,0xa5,sizeof(*out));out->bssid[0]=2;out->channel=36;out->rssi=-45;out->security=LUME_BSS_RSN;
    out->ssid_bytes=f->scan_mode==3?33:4;memcpy(out->ssid,"test",4);
    if(f->scan_mode==4)out->bssid[0]=1;
    if(f->scan_mode==5)out->channel=0;
    if(f->scan_mode==6){struct lume_wifi_bss_info nested;CHECK(lume_wireless_scan(f->index,&nested,1)==-LUME_EBUSY);}
    return 1;
}
static bool connect(void *ctx,const unsigned char *s,size_t sn,const unsigned char *k,size_t kn)
{struct fake *f=ctx;++f->connects;CHECK(sn==4 && !memcmp(s,"test",4));CHECK(kn==8 && !memcmp(k,"password",8));f->link=true;return !f->fail_connect;}
static void disconnect(void *ctx){struct fake *f=ctx;++f->disconnects;f->link=false;}
static void peer(void *ctx,unsigned char out[6]){(void)ctx;memset(out,0x42,6);}
static const struct lume_wireless_ops wifi_ops={scan,connect,disconnect,peer};
/* The dispatcher sees stack services only through the generic boundary. */
void lume_net_poll(void){for(unsigned i=0;i<lume_netif_count();++i)lume_netif_poll(lume_netif_at(i));}
int lume_net_snapshot(uint32_t index,struct lume_netif_info *out)
{struct lume_netif *p=lume_netif_find(index);if(!p)return -LUME_ENOENT;lume_netif_info(p,out);return 0;}
void lume_net_release(uint32_t index){CHECK(lume_netif_find(index)!=0);++releases;}
bool lume_net_dhcp(uint32_t index){CHECK(lume_netif_find(index)!=0);++dhcps;return true;}
static struct lume_net_control request;
static void init(uint32_t index){memset(&request,0,sizeof(request));request.version=1;request.ifindex=index;}
static struct lume_netif_descriptor desc(const char *name,unsigned kind,unsigned mac,struct fake *f)
{struct lume_netif_descriptor d={.name=name,.kind=kind,.mtu=1500,.mac={2,0,0,0,0,0},.ops=&data_ops,.context=f};d.mac[5]=(unsigned char)mac;return d;}
int main(void)
{
    CHECK(lume_netif_count()==0 && !lume_netif_at(0) && !lume_netif_find(0));
    struct fake eth={0},wifi={0},limited={0},eth2={0};
    struct lume_netif_descriptor d=desc("eth0",LUME_IF_ETHERNET,1,&eth),bad=d;
    CHECK(lume_netif_register(0)==-LUME_EINVAL);
    bad.name="";CHECK(lume_netif_register(&bad)==-LUME_EINVAL);
    bad=d;bad.name="../eth";CHECK(lume_netif_register(&bad)==-LUME_EINVAL);
    bad=d;bad.name="abcdefghijklmnop";CHECK(lume_netif_register(&bad)==-LUME_EINVAL);
    bad=d;bad.kind=99;CHECK(lume_netif_register(&bad)==-LUME_EINVAL);
    bad=d;bad.mtu=1501;CHECK(lume_netif_register(&bad)==-LUME_EINVAL);
    bad=d;bad.mtu=67;CHECK(lume_netif_register(&bad)==-LUME_EINVAL);
    bad=d;memset(bad.mac,0,6);CHECK(lume_netif_register(&bad)==-LUME_EINVAL);
    bad=d;bad.mac[0]=1;CHECK(lume_netif_register(&bad)==-LUME_EINVAL);
    bad=d;bad.ops=0;CHECK(lume_netif_register(&bad)==-LUME_EINVAL);
    eth.index=(uint32_t)lume_netif_register(&d);CHECK(eth.index==1);
    CHECK(lume_netif_register(&d)==-LUME_EEXIST);
    bad=d;bad.name="eth1";CHECK(lume_netif_register(&bad)==-LUME_EEXIST);
    d=desc("wlan0",LUME_IF_WIFI,2,&wifi);wifi.index=(uint32_t)lume_netif_register(&d);CHECK(wifi.index==2);
    struct lume_wifi_caps caps={7,3,32,32,{'P','T'},{0}},invalid=caps;
    CHECK(lume_wireless_register(0,&caps,&wifi_ops,&wifi)==-LUME_ENOENT);
    CHECK(lume_wireless_register(eth.index,&caps,&wifi_ops,&eth)==-LUME_EINVAL);
    invalid.reserved[2]=1;CHECK(lume_wireless_register(wifi.index,&invalid,&wifi_ops,&wifi)==-LUME_EINVAL);
    invalid=caps;invalid.bands=4;CHECK(lume_wireless_register(wifi.index,&invalid,&wifi_ops,&wifi)==-LUME_EINVAL);
    CHECK(lume_wireless_register(wifi.index,&caps,&wifi_ops,&wifi)==0);
    CHECK(lume_wireless_register(wifi.index,&caps,&wifi_ops,&wifi)==-LUME_EEXIST);
    struct lume_wifi_state state;CHECK(lume_wireless_state(2,&state)==0 && state.state==LUME_WIFI_DOWN);
    unsigned char frame[1514]={0};struct lume_netif *a=lume_netif_find(1),*b=lume_netif_find(2);
    CHECK(!lume_netif_transmit(a,frame,14) && eth.sent==0);
    CHECK(lume_netif_admin(a,true)==0);eth.link=true;lume_netif_poll(a);
    CHECK(lume_netif_transmit(a,frame,14) && eth.sent==1 && wifi.sent==0);
    CHECK(!lume_netif_transmit(a,frame,13));CHECK(!lume_netif_transmit(a,frame,1515));
    eth.fail_tx=true;CHECK(!lume_netif_transmit(a,frame,14));eth.fail_tx=false;
    eth.receive=42;CHECK(lume_netif_receive(a,frame,sizeof(frame))==42);
    eth.receive=1515;CHECK(lume_netif_receive(a,frame,sizeof(frame))==-LUME_EIO);
    eth.receive=13;CHECK(lume_netif_receive(a,frame,sizeof(frame))==-LUME_EIO);
    struct lume_netif_info info={0};lume_netif_info(a,&info);CHECK(info.rx_packets==1 && info.rx_errors==2 && info.tx_packets==1 && info.tx_errors==4);
    CHECK(lume_netif_admin(b,true)==0);
    struct lume_wifi_bss_info aps[32];
    for(int mode=0;mode<=6;++mode){wifi.scan_mode=mode;int n=lume_wireless_scan(2,aps,32);CHECK(n==((mode==0 || mode==6)?1:-LUME_EIO));}
    wifi.scan_mode=0;CHECK(lume_wireless_scan(2,aps,32)==1);
    for(unsigned i=0;i<16;++i)CHECK(aps[0].reserved[i]==0);
    for(unsigned i=4;i<32;++i)CHECK(aps[0].ssid[i]==0);
    struct lume_wifi_connect credentials={4,8,{'t','e','s','t'},{'p','a','s','s','w','o','r','d'}};
    struct lume_wifi_connect broken=credentials;broken.key_bytes=64;CHECK(lume_wireless_connect(2,&broken)==-LUME_EINVAL && wifi.connects==0);
    CHECK(lume_wireless_connect(2,&credentials)==0);
    CHECK(lume_wireless_state(2,&state)==0 && state.state==LUME_WIFI_CONNECTED && state.ssid_bytes==4 && !memcmp(state.ssid,"test",4));
    CHECK(lume_netif_transmit(b,frame,1514) && wifi.sent==1 && eth.sent==2);
    CHECK(lume_wireless_disconnect(2)==0 && !wifi.link);
    wifi.fail_connect=true;CHECK(lume_wireless_connect(2,&credentials)==-LUME_EIO && !wifi.link);
    CHECK(lume_wireless_state(2,&state)==0 && state.state==LUME_WIFI_FAILED);wifi.fail_connect=false;
    /* Dispatch validation must precede state changes, and outputs never echo keys. */
    init(0);CHECK(lume_net_control(LUME_NET_ENUM,&request)==0 && request.count==2);
    CHECK(request.data.interfaces[0].ifindex==1 && request.data.interfaces[1].ifindex==2);
    init(0);request.version=2;CHECK(lume_net_control(0,&request)==-LUME_EINVAL);
    init(0);request.reserved=1;CHECK(lume_net_control(0,&request)==-LUME_EINVAL);
    init(0);request.count=1;CHECK(lume_net_control(0,&request)==-LUME_EINVAL);
    init(0);CHECK(lume_net_control(10,&request)==-LUME_EINVAL);
    init(99);CHECK(lume_net_control(LUME_NET_GET,&request)==-LUME_ENOENT);
    init(2);request.data.admin_up=2;unsigned before=releases;CHECK(lume_net_control(LUME_NET_ADMIN,&request)==-LUME_EINVAL && releases==before);
    init(2);request.data.connect=broken;CHECK(lume_net_control(LUME_NET_WIFI_CONNECT,&request)==-LUME_EINVAL && releases==before);
    init(1);request.data.connect=credentials;CHECK(lume_net_control(LUME_NET_WIFI_CONNECT,&request)==-LUME_ENOENT && releases==before);
    init(2);request.data.connect=credentials;CHECK(lume_net_control(LUME_NET_WIFI_CONNECT,&request)==0 && request.data.wireless.state==LUME_WIFI_CONNECTED);
    for(unsigned i=sizeof(struct lume_wifi_state);i<sizeof(request.data);++i)CHECK(((unsigned char *)&request.data)[i]==0);
    init(2);CHECK(lume_net_control(LUME_NET_DHCP_START,&request)==0 && dhcps==1);
    init(2);CHECK(lume_net_control(LUME_NET_WIFI_DISCONNECT,&request)==0 && !wifi.link);
    init(2);CHECK(lume_net_control(LUME_NET_ADMIN,&request)==0);
    before=releases;init(2);request.data.connect=credentials;CHECK(lume_net_control(LUME_NET_WIFI_CONNECT,&request)==-LUME_EPERM && releases==before);
    CHECK(lume_wireless_scan(2,aps,1)==-LUME_EPERM);
    /* A second Wi-Fi backend may expose fewer capabilities. */
    d=desc("wlan1",LUME_IF_WIFI,3,&limited);limited.index=(uint32_t)lume_netif_register(&d);CHECK(limited.index==3);
    caps.features=LUME_WIFI_STATION;CHECK(lume_wireless_register(3,&caps,&wifi_ops,&limited)==0);
    CHECK(lume_netif_admin(lume_netif_find(3),true)==0);
    CHECK(lume_wireless_scan(3,aps,1)==-LUME_ENOSYS && !limited.scans);
    init(3);request.data.connect=credentials;before=releases;CHECK(lume_net_control(LUME_NET_WIFI_CONNECT,&request)==-LUME_ENOSYS && releases==before && !limited.connects);
    d=desc("eth1",LUME_IF_ETHERNET,4,&eth2);CHECK(lume_netif_register(&d)==4);
    d=desc("eth2",LUME_IF_ETHERNET,5,&eth2);CHECK(lume_netif_register(&d)==-LUME_ENOSPC && lume_netif_count()==4);
    /* Event cursors are per reader; reading never consumes another client's events. */
    for(unsigned i=0;i<80;++i){eth.link=!eth.link;lume_netif_sync(a);}
    struct lume_net_event one[16],two[16];uint64_t c1,c2;uint32_t lost1,lost2;
    int n1=lume_net_events(0,0,one,16,&c1,&lost1),n2=lume_net_events(0,0,two,16,&c2,&lost2);
    CHECK(n1==16 && n2==n1 && c1==c2 && lost1>0 && lost1==lost2 && !memcmp(one,two,sizeof(one)));
    CHECK(lume_net_events(0,c1,one,16,&c1,&lost1)==16 && lost1==0);
    CHECK(lume_net_events(0,c1,one,16,&c1,&lost1)==0);
    CHECK(lume_net_events(2,0,two,16,&c2,&lost2)==0 && c2==c1);
    CHECK(lume_net_events(0,UINT64_MAX,one,16,&c1,&lost1)==-LUME_EINVAL);
    for(unsigned i=0;i<16;++i)for(unsigned j=0;j<32;++j)CHECK(two[i].reserved[j]==0);
    CHECK(polls>0);printf("PASS GENERIC NETIF WIRELESS CONTROL EVENTS %u checks\n",checks);return 0;
}
