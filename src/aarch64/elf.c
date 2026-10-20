#include "lume/elf.h"

static uint16_t le16(const unsigned char *p) { return (uint16_t)p[0] | (uint16_t)p[1]<<8; }
static uint32_t le32(const unsigned char *p) { return le16(p) | (uint32_t)le16(p+2)<<16; }
static uint64_t le64(const unsigned char *p) { return le32(p) | (uint64_t)le32(p+4)<<32; }
static bool bounded(uint64_t offset, uint64_t bytes, uint64_t size)
{
    return offset <= size && bytes <= size-offset;
}

bool lume_elf_plan(const void *data, size_t size, struct lume_elf_plan *plan)
{
    const unsigned char *b=data;
    *plan=(struct lume_elf_plan){0};
    if (size<64 || b[0]!=0x7f || b[1]!='E' || b[2]!='L' || b[3]!='F' ||
        b[4]!=2 || b[5]!=1 || b[6]!=1 || b[7]!=0 || b[8]!=0 ||
        le16(b+16)!=2 || le16(b+18)!=183 || le32(b+20)!=1 ||
        le32(b+48)!=0 || le16(b+52)!=64 || le16(b+54)!=56)
        return false;
    unsigned headers=le16(b+56);
    uint64_t phoff=le64(b+32), entry=le64(b+24);
    if (!headers || headers>16 || phoff<64 || !bounded(phoff,(uint64_t)headers*56,size))
        return false;
    bool executable_entry=false;
    for (unsigned n=0;n<headers;++n) {
        const unsigned char *h=b+phoff+n*56;
        unsigned type=le32(h), flags=le32(h+4);
        if (type==0 || type==4 || type==6) continue;
        if (type==0x6474e551) { if (flags&1) return false; continue; }
        if (type!=1 || plan->count==LUME_ELF_SEGMENTS) return false;
        struct lume_elf_segment s={le64(h+8),le64(h+16),le64(h+32),le64(h+40),flags};
        uint64_t align=le64(h+48);
        if ((flags&~7u) || !(flags&4) || (flags&3)==3 || !s.memory_size ||
            s.file_size>s.memory_size || !bounded(s.offset,s.file_size,size) ||
            s.address<LUME_ELF_BASE || !bounded(s.address,s.memory_size,LUME_ELF_LIMIT) ||
            (s.address&4095)!=(s.offset&4095) ||
            (align>1 && ((align&(align-1)) || align>0x200000 ||
                         (s.address&(align-1))!=(s.offset&(align-1)))))
            return false;
        uint64_t first=s.address&~UINT64_C(4095), end=(s.address+s.memory_size+4095)&~UINT64_C(4095);
        unsigned pages=(unsigned)((end-first)/4096);
        if (pages>LUME_ELF_PAGES-plan->pages) return false;
        for (unsigned i=0;i<plan->count;++i) {
            struct lume_elf_segment *old=&plan->segments[i];
            uint64_t a=old->address&~UINT64_C(4095), z=(old->address+old->memory_size+4095)&~UINT64_C(4095);
            if (first<z && a<end) return false; /* No page permission unions. */
        }
        if ((flags&1) && entry>=s.address && entry-s.address<s.file_size)
            executable_entry=true;
        plan->segments[plan->count++]=s;
        plan->pages+=pages;
    }
    if (!plan->count || !executable_entry) return false;
    plan->entry=entry;
    return true;
}

void lume_elf_release(struct lume_elf_image *image)
{
    for (unsigned i=0;i<image->count;++i) {
        lume_mmu_unmap_user(image->task,image->pages[i].address);
        if (!lume_page_free(image->pages[i].physical)) lume_fail("ELF frame release");
    }
    *image=(struct lume_elf_image){0};
}

bool lume_elf_load(unsigned task, const void *data, size_t size, struct lume_elf_image *image)
{
    struct lume_elf_plan plan;
    *image=(struct lume_elf_image){0};
    if (!lume_elf_plan(data,size,&plan)) return false;
    image->task=task;
    for (unsigned n=0;n<plan.count;++n) {
        struct lume_elf_segment *s=&plan.segments[n];
        uint64_t first=s->address&~UINT64_C(4095), end=(s->address+s->memory_size+4095)&~UINT64_C(4095);
        for (uint64_t va=first;va<end;va+=4096) {
            uint64_t page=lume_page_alloc(0);
            if (!page) goto fail;
            if (!lume_mmu_map_user(task,va,page,(s->flags&2)!=0,(s->flags&1)!=0)) {
                (void)lume_page_free(page); goto fail;
            }
            image->pages[image->count].address=va;
            image->pages[image->count++].physical=page;
            uint64_t copy_start=va>s->address?va:s->address;
            uint64_t file_end=s->address+s->file_size;
            uint64_t copy_end=va+4096<file_end?va+4096:file_end;
            if (copy_start<copy_end) {
                unsigned char *out=(void *)(uintptr_t)(page+copy_start-va);
                const unsigned char *in=(const unsigned char *)data+s->offset+copy_start-s->address;
                for (uint64_t i=0;i<copy_end-copy_start;++i) out[i]=in[i];
            }
            if (s->flags&1) lume_sync_code(page,4096);
        }
    }
    image->entry=plan.entry;
    return true;
fail:
    lume_elf_release(image);
    return false;
}
