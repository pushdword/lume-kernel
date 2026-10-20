#include "lume/usb.h"
#include "lume/xhci_ring.h"
#include "lume/abi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
#define T(x) do{assert(x);++checks;}while(0)
static void press(struct lume_hid *h,uint8_t key,uint8_t modifiers) {
    uint8_t report[8]={modifiers,0,key,0,0,0,0,0},up[8]={0};
    lume_hid_report(h,report,8,0);lume_hid_report(h,up,8,1);
}
int main(void)
{
    uint8_t config[]={9,2,34,0,1,1,0,0x80,50,9,4,0,0,1,3,1,1,0,
        9,0x21,0x11,1,0,1,0x22,63,0,7,5,0x81,3,8,0,10};
    struct lume_usb_interface i;
    T(lume_usb_configuration(config,sizeof(config),&i));
    T(i.keyboard && !i.hub && i.endpoint==0x81 && i.packet==8 && i.interval==10);
    for(unsigned n=0;n<sizeof(config);++n)T(!lume_usb_configuration(config,n,&i));
    for(unsigned n=0;n<2;++n){uint8_t b[sizeof(config)];memcpy(b,config,sizeof(b));b[18]=n;T(!lume_usb_configuration(b,sizeof(b),&i));}
    uint8_t bad[sizeof(config)];memcpy(bad,config,sizeof(bad));bad[18]=255;T(!lume_usb_configuration(bad,sizeof(bad),&i));
    memcpy(bad,config,sizeof(bad));bad[16]=0;T(lume_usb_configuration(bad,sizeof(bad),&i) && !i.keyboard);
    memcpy(bad,config,sizeof(bad));bad[29]=1;T(lume_usb_configuration(bad,sizeof(bad),&i) && !i.keyboard);
    memcpy(bad,config,sizeof(bad));bad[31]=7;T(lume_usb_configuration(bad,sizeof(bad),&i) && !i.keyboard);
    memcpy(bad,config,sizeof(bad));bad[33]=0;T(lume_usb_configuration(bad,sizeof(bad),&i) && !i.keyboard);
    uint8_t hub[]={9,2,25,0,1,1,0,0xe0,0,9,4,0,0,1,9,0,0,0,7,5,0x81,3,1,0,12};
    T(lume_usb_configuration(hub,sizeof(hub),&i) && i.hub && !i.keyboard);
    const uint8_t rpi_keyboard[]={9,2,59,0,2,1,0,0xa0,50,9,4,0,0,1,3,1,1,0,
        9,0x21,0x11,1,0,1,0x22,65,0,7,5,0x81,3,8,0,10,
        9,4,1,0,1,3,0,0,0,9,0x21,0x11,1,0,1,0x22,59,0,7,5,0x82,3,5,0,10};
    T(lume_usb_configuration(rpi_keyboard,sizeof(rpi_keyboard),&i) && i.keyboard && i.interface==0 && i.endpoint==0x81);
    const uint8_t rpi_hub[]={9,2,41,0,1,1,0,0xe0,50,9,4,0,0,1,9,0,1,0,
        7,5,0x81,3,1,0,12,9,4,0,1,1,9,0,2,0,7,5,0x81,3,1,0,12};
    T(lume_usb_configuration(rpi_hub,sizeof(rpi_hub),&i) && i.hub && !i.keyboard);
    struct lume_xhci_trb trbs[256]={0};
    struct lume_xhci_ring ring={trbs,UINT64_C(0x1100020000),0,1};
    for(unsigned n=0;n<1020;++n) {
        unsigned index=n%255,cycle=1^((n/255)&1);
        uint64_t where=lume_xhci_enqueue(&ring,n,8,(1u<<10)|32);
        T(where==ring.dma+index*16 && trbs[index].parameter==n && (trbs[index].control&1)==cycle);
        if(index==254)T(trbs[255].parameter==ring.dma && trbs[255].control==((6u<<10)|2|cycle));
    }
    struct lume_hid h={0};
    press(&h,4,0);T(lume_hid_getc(&h)=='a');
    press(&h,4,2);T(lume_hid_getc(&h)=='A');
    press(&h,57,0);press(&h,4,0);T(lume_hid_getc(&h)=='A');
    press(&h,4,2);T(lume_hid_getc(&h)=='a');press(&h,57,0);
    press(&h,36,2);T(lume_hid_getc(&h)=='/');
    /* Exercise every numeric-row index with both Shift keys. This also
       covers the conditional-array access reported by GCC's sanitizer. */
    const char digits[]="1234567890",symbols[]="!\"#$%&/()=";
    for(unsigned n=0;n<10;++n) {
        press(&h,30+n,0);T(lume_hid_getc(&h)==digits[n]);
        press(&h,30+n,2);T(lume_hid_getc(&h)==symbols[n]);
        press(&h,30+n,0x20);T(lume_hid_getc(&h)==symbols[n]);
        T(lume_hid_getc(&h)==-LUME_EAGAIN);
    }
    press(&h,100,2);T(lume_hid_getc(&h)=='>');
    press(&h,31,0x40);T(lume_hid_getc(&h)=='@');
    press(&h,37,0x40);T(lume_hid_getc(&h)=='[');
    press(&h,6,1);T(lume_hid_getc(&h)==3);
    press(&h,51,0);T(lume_hid_getc(&h)==0xc3 && lume_hid_getc(&h)==0xa7);
    press(&h,48,0);T(lume_hid_getc(&h)==-LUME_EAGAIN);
    press(&h,4,0);T(lume_hid_getc(&h)==0xc3 && lume_hid_getc(&h)==0xa1);
    press(&h,49,0);press(&h,18,0);T(lume_hid_getc(&h)==0xc3 && lume_hid_getc(&h)==0xb5);
    uint8_t report[8]={0,0,4,4,0,0,0,0},up[8]={0};
    lume_hid_report(&h,report,8,10);lume_hid_report(&h,report,8,20);
    T(lume_hid_getc(&h)=='a' && lume_hid_getc(&h)==-LUME_EAGAIN);
    lume_hid_repeat(&h,509);T(lume_hid_getc(&h)==-LUME_EAGAIN);
    lume_hid_repeat(&h,510);T(lume_hid_getc(&h)=='a');
    lume_hid_report(&h,up,8,520);lume_hid_repeat(&h,1000);T(lume_hid_getc(&h)==-LUME_EAGAIN);
    report[2]=1;lume_hid_report(&h,report,8,1000);T(lume_hid_getc(&h)==-LUME_EAGAIN);
    for(unsigned n=0;n<1000;++n){press(&h,5,0);T(lume_hid_getc(&h)=='b');}
    for(unsigned n=0;n<256;++n)press(&h,5,0);
    unsigned dropped=h.dropped;press(&h,51,0);T(h.dropped==dropped+1);
    for(unsigned n=0;n<256;++n)T(lume_hid_getc(&h)=='b');
    T(lume_hid_getc(&h)==-LUME_EAGAIN);
    printf("PASS_USB_DESCRIPTOR_RING_HID_PT checks=%u\n",checks);
}
