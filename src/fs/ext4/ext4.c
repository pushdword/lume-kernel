#include "checksum.h"
#include "htree.h"
#include "jbd2.h"
#include "lume/ext4.h"
#include <stdbool.h>
#include <string.h>

#define EXTENTS UINT32_C(0x80000)
#define METADATA_CSUM UINT32_C(0x400)
#define HUGE_FILE UINT32_C(0x8)
#define CSUM_SEED UINT32_C(0x2000)
#define BIT64 UINT32_C(0x80)

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;
}

/* Castagnoli polynomial 0x82f63b78. Each slice advances one zero byte. */
// clang-format off
static const uint32_t crc_slices[4][256] = {
    {
        0x00000000u, 0xf26b8303u, 0xe13b70f7u, 0x1350f3f4u, 0xc79a971fu, 0x35f1141cu,
        0x26a1e7e8u, 0xd4ca64ebu, 0x8ad958cfu, 0x78b2dbccu, 0x6be22838u, 0x9989ab3bu,
        0x4d43cfd0u, 0xbf284cd3u, 0xac78bf27u, 0x5e133c24u, 0x105ec76fu, 0xe235446cu,
        0xf165b798u, 0x030e349bu, 0xd7c45070u, 0x25afd373u, 0x36ff2087u, 0xc494a384u,
        0x9a879fa0u, 0x68ec1ca3u, 0x7bbcef57u, 0x89d76c54u, 0x5d1d08bfu, 0xaf768bbcu,
        0xbc267848u, 0x4e4dfb4bu, 0x20bd8edeu, 0xd2d60dddu, 0xc186fe29u, 0x33ed7d2au,
        0xe72719c1u, 0x154c9ac2u, 0x061c6936u, 0xf477ea35u, 0xaa64d611u, 0x580f5512u,
        0x4b5fa6e6u, 0xb93425e5u, 0x6dfe410eu, 0x9f95c20du, 0x8cc531f9u, 0x7eaeb2fau,
        0x30e349b1u, 0xc288cab2u, 0xd1d83946u, 0x23b3ba45u, 0xf779deaeu, 0x05125dadu,
        0x1642ae59u, 0xe4292d5au, 0xba3a117eu, 0x4851927du, 0x5b016189u, 0xa96ae28au,
        0x7da08661u, 0x8fcb0562u, 0x9c9bf696u, 0x6ef07595u, 0x417b1dbcu, 0xb3109ebfu,
        0xa0406d4bu, 0x522bee48u, 0x86e18aa3u, 0x748a09a0u, 0x67dafa54u, 0x95b17957u,
        0xcba24573u, 0x39c9c670u, 0x2a993584u, 0xd8f2b687u, 0x0c38d26cu, 0xfe53516fu,
        0xed03a29bu, 0x1f682198u, 0x5125dad3u, 0xa34e59d0u, 0xb01eaa24u, 0x42752927u,
        0x96bf4dccu, 0x64d4cecfu, 0x77843d3bu, 0x85efbe38u, 0xdbfc821cu, 0x2997011fu,
        0x3ac7f2ebu, 0xc8ac71e8u, 0x1c661503u, 0xee0d9600u, 0xfd5d65f4u, 0x0f36e6f7u,
        0x61c69362u, 0x93ad1061u, 0x80fde395u, 0x72966096u, 0xa65c047du, 0x5437877eu,
        0x4767748au, 0xb50cf789u, 0xeb1fcbadu, 0x197448aeu, 0x0a24bb5au, 0xf84f3859u,
        0x2c855cb2u, 0xdeeedfb1u, 0xcdbe2c45u, 0x3fd5af46u, 0x7198540du, 0x83f3d70eu,
        0x90a324fau, 0x62c8a7f9u, 0xb602c312u, 0x44694011u, 0x5739b3e5u, 0xa55230e6u,
        0xfb410cc2u, 0x092a8fc1u, 0x1a7a7c35u, 0xe811ff36u, 0x3cdb9bddu, 0xceb018deu,
        0xdde0eb2au, 0x2f8b6829u, 0x82f63b78u, 0x709db87bu, 0x63cd4b8fu, 0x91a6c88cu,
        0x456cac67u, 0xb7072f64u, 0xa457dc90u, 0x563c5f93u, 0x082f63b7u, 0xfa44e0b4u,
        0xe9141340u, 0x1b7f9043u, 0xcfb5f4a8u, 0x3dde77abu, 0x2e8e845fu, 0xdce5075cu,
        0x92a8fc17u, 0x60c37f14u, 0x73938ce0u, 0x81f80fe3u, 0x55326b08u, 0xa759e80bu,
        0xb4091bffu, 0x466298fcu, 0x1871a4d8u, 0xea1a27dbu, 0xf94ad42fu, 0x0b21572cu,
        0xdfeb33c7u, 0x2d80b0c4u, 0x3ed04330u, 0xccbbc033u, 0xa24bb5a6u, 0x502036a5u,
        0x4370c551u, 0xb11b4652u, 0x65d122b9u, 0x97baa1bau, 0x84ea524eu, 0x7681d14du,
        0x2892ed69u, 0xdaf96e6au, 0xc9a99d9eu, 0x3bc21e9du, 0xef087a76u, 0x1d63f975u,
        0x0e330a81u, 0xfc588982u, 0xb21572c9u, 0x407ef1cau, 0x532e023eu, 0xa145813du,
        0x758fe5d6u, 0x87e466d5u, 0x94b49521u, 0x66df1622u, 0x38cc2a06u, 0xcaa7a905u,
        0xd9f75af1u, 0x2b9cd9f2u, 0xff56bd19u, 0x0d3d3e1au, 0x1e6dcdeeu, 0xec064eedu,
        0xc38d26c4u, 0x31e6a5c7u, 0x22b65633u, 0xd0ddd530u, 0x0417b1dbu, 0xf67c32d8u,
        0xe52cc12cu, 0x1747422fu, 0x49547e0bu, 0xbb3ffd08u, 0xa86f0efcu, 0x5a048dffu,
        0x8ecee914u, 0x7ca56a17u, 0x6ff599e3u, 0x9d9e1ae0u, 0xd3d3e1abu, 0x21b862a8u,
        0x32e8915cu, 0xc083125fu, 0x144976b4u, 0xe622f5b7u, 0xf5720643u, 0x07198540u,
        0x590ab964u, 0xab613a67u, 0xb831c993u, 0x4a5a4a90u, 0x9e902e7bu, 0x6cfbad78u,
        0x7fab5e8cu, 0x8dc0dd8fu, 0xe330a81au, 0x115b2b19u, 0x020bd8edu, 0xf0605beeu,
        0x24aa3f05u, 0xd6c1bc06u, 0xc5914ff2u, 0x37faccf1u, 0x69e9f0d5u, 0x9b8273d6u,
        0x88d28022u, 0x7ab90321u, 0xae7367cau, 0x5c18e4c9u, 0x4f48173du, 0xbd23943eu,
        0xf36e6f75u, 0x0105ec76u, 0x12551f82u, 0xe03e9c81u, 0x34f4f86au, 0xc69f7b69u,
        0xd5cf889du, 0x27a40b9eu, 0x79b737bau, 0x8bdcb4b9u, 0x988c474du, 0x6ae7c44eu,
        0xbe2da0a5u, 0x4c4623a6u, 0x5f16d052u, 0xad7d5351u,
    },
    {
        0x00000000u, 0x13a29877u, 0x274530eeu, 0x34e7a899u, 0x4e8a61dcu, 0x5d28f9abu,
        0x69cf5132u, 0x7a6dc945u, 0x9d14c3b8u, 0x8eb65bcfu, 0xba51f356u, 0xa9f36b21u,
        0xd39ea264u, 0xc03c3a13u, 0xf4db928au, 0xe7790afdu, 0x3fc5f181u, 0x2c6769f6u,
        0x1880c16fu, 0x0b225918u, 0x714f905du, 0x62ed082au, 0x560aa0b3u, 0x45a838c4u,
        0xa2d13239u, 0xb173aa4eu, 0x859402d7u, 0x96369aa0u, 0xec5b53e5u, 0xfff9cb92u,
        0xcb1e630bu, 0xd8bcfb7cu, 0x7f8be302u, 0x6c297b75u, 0x58ced3ecu, 0x4b6c4b9bu,
        0x310182deu, 0x22a31aa9u, 0x1644b230u, 0x05e62a47u, 0xe29f20bau, 0xf13db8cdu,
        0xc5da1054u, 0xd6788823u, 0xac154166u, 0xbfb7d911u, 0x8b507188u, 0x98f2e9ffu,
        0x404e1283u, 0x53ec8af4u, 0x670b226du, 0x74a9ba1au, 0x0ec4735fu, 0x1d66eb28u,
        0x298143b1u, 0x3a23dbc6u, 0xdd5ad13bu, 0xcef8494cu, 0xfa1fe1d5u, 0xe9bd79a2u,
        0x93d0b0e7u, 0x80722890u, 0xb4958009u, 0xa737187eu, 0xff17c604u, 0xecb55e73u,
        0xd852f6eau, 0xcbf06e9du, 0xb19da7d8u, 0xa23f3fafu, 0x96d89736u, 0x857a0f41u,
        0x620305bcu, 0x71a19dcbu, 0x45463552u, 0x56e4ad25u, 0x2c896460u, 0x3f2bfc17u,
        0x0bcc548eu, 0x186eccf9u, 0xc0d23785u, 0xd370aff2u, 0xe797076bu, 0xf4359f1cu,
        0x8e585659u, 0x9dface2eu, 0xa91d66b7u, 0xbabffec0u, 0x5dc6f43du, 0x4e646c4au,
        0x7a83c4d3u, 0x69215ca4u, 0x134c95e1u, 0x00ee0d96u, 0x3409a50fu, 0x27ab3d78u,
        0x809c2506u, 0x933ebd71u, 0xa7d915e8u, 0xb47b8d9fu, 0xce1644dau, 0xddb4dcadu,
        0xe9537434u, 0xfaf1ec43u, 0x1d88e6beu, 0x0e2a7ec9u, 0x3acdd650u, 0x296f4e27u,
        0x53028762u, 0x40a01f15u, 0x7447b78cu, 0x67e52ffbu, 0xbf59d487u, 0xacfb4cf0u,
        0x981ce469u, 0x8bbe7c1eu, 0xf1d3b55bu, 0xe2712d2cu, 0xd69685b5u, 0xc5341dc2u,
        0x224d173fu, 0x31ef8f48u, 0x050827d1u, 0x16aabfa6u, 0x6cc776e3u, 0x7f65ee94u,
        0x4b82460du, 0x5820de7au, 0xfbc3faf9u, 0xe861628eu, 0xdc86ca17u, 0xcf245260u,
        0xb5499b25u, 0xa6eb0352u, 0x920cabcbu, 0x81ae33bcu, 0x66d73941u, 0x7575a136u,
        0x419209afu, 0x523091d8u, 0x285d589du, 0x3bffc0eau, 0x0f186873u, 0x1cbaf004u,
        0xc4060b78u, 0xd7a4930fu, 0xe3433b96u, 0xf0e1a3e1u, 0x8a8c6aa4u, 0x992ef2d3u,
        0xadc95a4au, 0xbe6bc23du, 0x5912c8c0u, 0x4ab050b7u, 0x7e57f82eu, 0x6df56059u,
        0x1798a91cu, 0x043a316bu, 0x30dd99f2u, 0x237f0185u, 0x844819fbu, 0x97ea818cu,
        0xa30d2915u, 0xb0afb162u, 0xcac27827u, 0xd960e050u, 0xed8748c9u, 0xfe25d0beu,
        0x195cda43u, 0x0afe4234u, 0x3e19eaadu, 0x2dbb72dau, 0x57d6bb9fu, 0x447423e8u,
        0x70938b71u, 0x63311306u, 0xbb8de87au, 0xa82f700du, 0x9cc8d894u, 0x8f6a40e3u,
        0xf50789a6u, 0xe6a511d1u, 0xd242b948u, 0xc1e0213fu, 0x26992bc2u, 0x353bb3b5u,
        0x01dc1b2cu, 0x127e835bu, 0x68134a1eu, 0x7bb1d269u, 0x4f567af0u, 0x5cf4e287u,
        0x04d43cfdu, 0x1776a48au, 0x23910c13u, 0x30339464u, 0x4a5e5d21u, 0x59fcc556u,
        0x6d1b6dcfu, 0x7eb9f5b8u, 0x99c0ff45u, 0x8a626732u, 0xbe85cfabu, 0xad2757dcu,
        0xd74a9e99u, 0xc4e806eeu, 0xf00fae77u, 0xe3ad3600u, 0x3b11cd7cu, 0x28b3550bu,
        0x1c54fd92u, 0x0ff665e5u, 0x759baca0u, 0x663934d7u, 0x52de9c4eu, 0x417c0439u,
        0xa6050ec4u, 0xb5a796b3u, 0x81403e2au, 0x92e2a65du, 0xe88f6f18u, 0xfb2df76fu,
        0xcfca5ff6u, 0xdc68c781u, 0x7b5fdfffu, 0x68fd4788u, 0x5c1aef11u, 0x4fb87766u,
        0x35d5be23u, 0x26772654u, 0x12908ecdu, 0x013216bau, 0xe64b1c47u, 0xf5e98430u,
        0xc10e2ca9u, 0xd2acb4deu, 0xa8c17d9bu, 0xbb63e5ecu, 0x8f844d75u, 0x9c26d502u,
        0x449a2e7eu, 0x5738b609u, 0x63df1e90u, 0x707d86e7u, 0x0a104fa2u, 0x19b2d7d5u,
        0x2d557f4cu, 0x3ef7e73bu, 0xd98eedc6u, 0xca2c75b1u, 0xfecbdd28u, 0xed69455fu,
        0x97048c1au, 0x84a6146du, 0xb041bcf4u, 0xa3e32483u,
    },
    {
        0x00000000u, 0xa541927eu, 0x4f6f520du, 0xea2ec073u, 0x9edea41au, 0x3b9f3664u,
        0xd1b1f617u, 0x74f06469u, 0x38513ec5u, 0x9d10acbbu, 0x773e6cc8u, 0xd27ffeb6u,
        0xa68f9adfu, 0x03ce08a1u, 0xe9e0c8d2u, 0x4ca15aacu, 0x70a27d8au, 0xd5e3eff4u,
        0x3fcd2f87u, 0x9a8cbdf9u, 0xee7cd990u, 0x4b3d4beeu, 0xa1138b9du, 0x045219e3u,
        0x48f3434fu, 0xedb2d131u, 0x079c1142u, 0xa2dd833cu, 0xd62de755u, 0x736c752bu,
        0x9942b558u, 0x3c032726u, 0xe144fb14u, 0x4405696au, 0xae2ba919u, 0x0b6a3b67u,
        0x7f9a5f0eu, 0xdadbcd70u, 0x30f50d03u, 0x95b49f7du, 0xd915c5d1u, 0x7c5457afu,
        0x967a97dcu, 0x333b05a2u, 0x47cb61cbu, 0xe28af3b5u, 0x08a433c6u, 0xade5a1b8u,
        0x91e6869eu, 0x34a714e0u, 0xde89d493u, 0x7bc846edu, 0x0f382284u, 0xaa79b0fau,
        0x40577089u, 0xe516e2f7u, 0xa9b7b85bu, 0x0cf62a25u, 0xe6d8ea56u, 0x43997828u,
        0x37691c41u, 0x92288e3fu, 0x78064e4cu, 0xdd47dc32u, 0xc76580d9u, 0x622412a7u,
        0x880ad2d4u, 0x2d4b40aau, 0x59bb24c3u, 0xfcfab6bdu, 0x16d476ceu, 0xb395e4b0u,
        0xff34be1cu, 0x5a752c62u, 0xb05bec11u, 0x151a7e6fu, 0x61ea1a06u, 0xc4ab8878u,
        0x2e85480bu, 0x8bc4da75u, 0xb7c7fd53u, 0x12866f2du, 0xf8a8af5eu, 0x5de93d20u,
        0x29195949u, 0x8c58cb37u, 0x66760b44u, 0xc337993au, 0x8f96c396u, 0x2ad751e8u,
        0xc0f9919bu, 0x65b803e5u, 0x1148678cu, 0xb409f5f2u, 0x5e273581u, 0xfb66a7ffu,
        0x26217bcdu, 0x8360e9b3u, 0x694e29c0u, 0xcc0fbbbeu, 0xb8ffdfd7u, 0x1dbe4da9u,
        0xf7908ddau, 0x52d11fa4u, 0x1e704508u, 0xbb31d776u, 0x511f1705u, 0xf45e857bu,
        0x80aee112u, 0x25ef736cu, 0xcfc1b31fu, 0x6a802161u, 0x56830647u, 0xf3c29439u,
        0x19ec544au, 0xbcadc634u, 0xc85da25du, 0x6d1c3023u, 0x8732f050u, 0x2273622eu,
        0x6ed23882u, 0xcb93aafcu, 0x21bd6a8fu, 0x84fcf8f1u, 0xf00c9c98u, 0x554d0ee6u,
        0xbf63ce95u, 0x1a225cebu, 0x8b277743u, 0x2e66e53du, 0xc448254eu, 0x6109b730u,
        0x15f9d359u, 0xb0b84127u, 0x5a968154u, 0xffd7132au, 0xb3764986u, 0x1637dbf8u,
        0xfc191b8bu, 0x595889f5u, 0x2da8ed9cu, 0x88e97fe2u, 0x62c7bf91u, 0xc7862defu,
        0xfb850ac9u, 0x5ec498b7u, 0xb4ea58c4u, 0x11abcabau, 0x655baed3u, 0xc01a3cadu,
        0x2a34fcdeu, 0x8f756ea0u, 0xc3d4340cu, 0x6695a672u, 0x8cbb6601u, 0x29faf47fu,
        0x5d0a9016u, 0xf84b0268u, 0x1265c21bu, 0xb7245065u, 0x6a638c57u, 0xcf221e29u,
        0x250cde5au, 0x804d4c24u, 0xf4bd284du, 0x51fcba33u, 0xbbd27a40u, 0x1e93e83eu,
        0x5232b292u, 0xf77320ecu, 0x1d5de09fu, 0xb81c72e1u, 0xccec1688u, 0x69ad84f6u,
        0x83834485u, 0x26c2d6fbu, 0x1ac1f1ddu, 0xbf8063a3u, 0x55aea3d0u, 0xf0ef31aeu,
        0x841f55c7u, 0x215ec7b9u, 0xcb7007cau, 0x6e3195b4u, 0x2290cf18u, 0x87d15d66u,
        0x6dff9d15u, 0xc8be0f6bu, 0xbc4e6b02u, 0x190ff97cu, 0xf321390fu, 0x5660ab71u,
        0x4c42f79au, 0xe90365e4u, 0x032da597u, 0xa66c37e9u, 0xd29c5380u, 0x77ddc1feu,
        0x9df3018du, 0x38b293f3u, 0x7413c95fu, 0xd1525b21u, 0x3b7c9b52u, 0x9e3d092cu,
        0xeacd6d45u, 0x4f8cff3bu, 0xa5a23f48u, 0x00e3ad36u, 0x3ce08a10u, 0x99a1186eu,
        0x738fd81du, 0xd6ce4a63u, 0xa23e2e0au, 0x077fbc74u, 0xed517c07u, 0x4810ee79u,
        0x04b1b4d5u, 0xa1f026abu, 0x4bdee6d8u, 0xee9f74a6u, 0x9a6f10cfu, 0x3f2e82b1u,
        0xd50042c2u, 0x7041d0bcu, 0xad060c8eu, 0x08479ef0u, 0xe2695e83u, 0x4728ccfdu,
        0x33d8a894u, 0x96993aeau, 0x7cb7fa99u, 0xd9f668e7u, 0x9557324bu, 0x3016a035u,
        0xda386046u, 0x7f79f238u, 0x0b899651u, 0xaec8042fu, 0x44e6c45cu, 0xe1a75622u,
        0xdda47104u, 0x78e5e37au, 0x92cb2309u, 0x378ab177u, 0x437ad51eu, 0xe63b4760u,
        0x0c158713u, 0xa954156du, 0xe5f54fc1u, 0x40b4ddbfu, 0xaa9a1dccu, 0x0fdb8fb2u,
        0x7b2bebdbu, 0xde6a79a5u, 0x3444b9d6u, 0x91052ba8u,
    },
    {
        0x00000000u, 0xdd45aab8u, 0xbf672381u, 0x62228939u, 0x7b2231f3u, 0xa6679b4bu,
        0xc4451272u, 0x1900b8cau, 0xf64463e6u, 0x2b01c95eu, 0x49234067u, 0x9466eadfu,
        0x8d665215u, 0x5023f8adu, 0x32017194u, 0xef44db2cu, 0xe964b13du, 0x34211b85u,
        0x560392bcu, 0x8b463804u, 0x924680ceu, 0x4f032a76u, 0x2d21a34fu, 0xf06409f7u,
        0x1f20d2dbu, 0xc2657863u, 0xa047f15au, 0x7d025be2u, 0x6402e328u, 0xb9474990u,
        0xdb65c0a9u, 0x06206a11u, 0xd725148bu, 0x0a60be33u, 0x6842370au, 0xb5079db2u,
        0xac072578u, 0x71428fc0u, 0x136006f9u, 0xce25ac41u, 0x2161776du, 0xfc24ddd5u,
        0x9e0654ecu, 0x4343fe54u, 0x5a43469eu, 0x8706ec26u, 0xe524651fu, 0x3861cfa7u,
        0x3e41a5b6u, 0xe3040f0eu, 0x81268637u, 0x5c632c8fu, 0x45639445u, 0x98263efdu,
        0xfa04b7c4u, 0x27411d7cu, 0xc805c650u, 0x15406ce8u, 0x7762e5d1u, 0xaa274f69u,
        0xb327f7a3u, 0x6e625d1bu, 0x0c40d422u, 0xd1057e9au, 0xaba65fe7u, 0x76e3f55fu,
        0x14c17c66u, 0xc984d6deu, 0xd0846e14u, 0x0dc1c4acu, 0x6fe34d95u, 0xb2a6e72du,
        0x5de23c01u, 0x80a796b9u, 0xe2851f80u, 0x3fc0b538u, 0x26c00df2u, 0xfb85a74au,
        0x99a72e73u, 0x44e284cbu, 0x42c2eedau, 0x9f874462u, 0xfda5cd5bu, 0x20e067e3u,
        0x39e0df29u, 0xe4a57591u, 0x8687fca8u, 0x5bc25610u, 0xb4868d3cu, 0x69c32784u,
        0x0be1aebdu, 0xd6a40405u, 0xcfa4bccfu, 0x12e11677u, 0x70c39f4eu, 0xad8635f6u,
        0x7c834b6cu, 0xa1c6e1d4u, 0xc3e468edu, 0x1ea1c255u, 0x07a17a9fu, 0xdae4d027u,
        0xb8c6591eu, 0x6583f3a6u, 0x8ac7288au, 0x57828232u, 0x35a00b0bu, 0xe8e5a1b3u,
        0xf1e51979u, 0x2ca0b3c1u, 0x4e823af8u, 0x93c79040u, 0x95e7fa51u, 0x48a250e9u,
        0x2a80d9d0u, 0xf7c57368u, 0xeec5cba2u, 0x3380611au, 0x51a2e823u, 0x8ce7429bu,
        0x63a399b7u, 0xbee6330fu, 0xdcc4ba36u, 0x0181108eu, 0x1881a844u, 0xc5c402fcu,
        0xa7e68bc5u, 0x7aa3217du, 0x52a0c93fu, 0x8fe56387u, 0xedc7eabeu, 0x30824006u,
        0x2982f8ccu, 0xf4c75274u, 0x96e5db4du, 0x4ba071f5u, 0xa4e4aad9u, 0x79a10061u,
        0x1b838958u, 0xc6c623e0u, 0xdfc69b2au, 0x02833192u, 0x60a1b8abu, 0xbde41213u,
        0xbbc47802u, 0x6681d2bau, 0x04a35b83u, 0xd9e6f13bu, 0xc0e649f1u, 0x1da3e349u,
        0x7f816a70u, 0xa2c4c0c8u, 0x4d801be4u, 0x90c5b15cu, 0xf2e73865u, 0x2fa292ddu,
        0x36a22a17u, 0xebe780afu, 0x89c50996u, 0x5480a32eu, 0x8585ddb4u, 0x58c0770cu,
        0x3ae2fe35u, 0xe7a7548du, 0xfea7ec47u, 0x23e246ffu, 0x41c0cfc6u, 0x9c85657eu,
        0x73c1be52u, 0xae8414eau, 0xcca69dd3u, 0x11e3376bu, 0x08e38fa1u, 0xd5a62519u,
        0xb784ac20u, 0x6ac10698u, 0x6ce16c89u, 0xb1a4c631u, 0xd3864f08u, 0x0ec3e5b0u,
        0x17c35d7au, 0xca86f7c2u, 0xa8a47efbu, 0x75e1d443u, 0x9aa50f6fu, 0x47e0a5d7u,
        0x25c22ceeu, 0xf8878656u, 0xe1873e9cu, 0x3cc29424u, 0x5ee01d1du, 0x83a5b7a5u,
        0xf90696d8u, 0x24433c60u, 0x4661b559u, 0x9b241fe1u, 0x8224a72bu, 0x5f610d93u,
        0x3d4384aau, 0xe0062e12u, 0x0f42f53eu, 0xd2075f86u, 0xb025d6bfu, 0x6d607c07u,
        0x7460c4cdu, 0xa9256e75u, 0xcb07e74cu, 0x16424df4u, 0x106227e5u, 0xcd278d5du,
        0xaf050464u, 0x7240aedcu, 0x6b401616u, 0xb605bcaeu, 0xd4273597u, 0x09629f2fu,
        0xe6264403u, 0x3b63eebbu, 0x59416782u, 0x8404cd3au, 0x9d0475f0u, 0x4041df48u,
        0x22635671u, 0xff26fcc9u, 0x2e238253u, 0xf36628ebu, 0x9144a1d2u, 0x4c010b6au,
        0x5501b3a0u, 0x88441918u, 0xea669021u, 0x37233a99u, 0xd867e1b5u, 0x05224b0du,
        0x6700c234u, 0xba45688cu, 0xa345d046u, 0x7e007afeu, 0x1c22f3c7u, 0xc167597fu,
        0xc747336eu, 0x1a0299d6u, 0x782010efu, 0xa565ba57u, 0xbc65029du, 0x6120a825u,
        0x0302211cu, 0xde478ba4u, 0x31035088u, 0xec46fa30u, 0x8e647309u, 0x5321d9b1u,
        0x4a21617bu, 0x9764cbc3u, 0xf54642fau, 0x2803e842u,
    },
};
// clang-format on

uint32_t lume_ext4_crc32c(uint32_t value, const unsigned char *data, size_t bytes)
{
    while (bytes >= 4)
    {
        value ^= le32(data);
        value = crc_slices[3][value & 0xff] ^ crc_slices[2][(value >> 8) & 0xff] ^
                crc_slices[1][(value >> 16) & 0xff] ^ crc_slices[0][value >> 24];
        data += 4;
        bytes -= 4;
    }
    while (bytes--)
        value = (value >> 8) ^ crc_slices[0][(value ^ *data++) & 0xff];
    return value;
}

static uint32_t crc_number(uint32_t value, uint32_t number)
{
    unsigned char p[4];
    for (unsigned i = 0; i != 4; ++i)
        p[i] = (unsigned char)(number >> (i * 8));
    return lume_ext4_crc32c(value, p, sizeof p);
}

static uint32_t inode_seed(const struct lume_ext4 *fs, const struct lume_ext4_inode *inode)
{
    return crc_number(crc_number(fs->checksum_seed, inode->number), inode->generation);
}

static int read_bytes(const struct lume_ext4 *fs, uint64_t offset, void *out, size_t bytes)
{
    if (offset > fs->bytes || bytes > fs->bytes - offset)
        return LUME_EXT4_INVALID;
    return fs->read(fs->context, offset, out, bytes) ? LUME_EXT4_IO : 0;
}

static int read_block(const struct lume_ext4 *fs, uint64_t block, void *out)
{
    if (!block || block >= fs->blocks)
        return LUME_EXT4_INVALID;
    return read_bytes(fs, block * fs->block_bytes, out, fs->block_bytes);
}

static int open_filesystem(struct lume_ext4 *fs, lume_ext4_read_bytes read, void *context,
                           uint64_t bytes, void *scratch, size_t scratch_bytes, bool pending)
{
    if (!fs || !read || !scratch || scratch_bytes < 4096)
        return LUME_EXT4_INVALID;
    *fs = (struct lume_ext4){0};
    if (bytes < 2048)
        return LUME_EXT4_INVALID;
    unsigned char *s = scratch;
    if (read(context, 1024, s, 1024))
        return LUME_EXT4_IO;
    if (le16(s + 0x38) != 0xef53 || le32(s + 0x4c) != 1)
        return LUME_EXT4_INVALID;
    uint32_t incompat = le32(s + 0x60), ro = le32(s + 0x64);
    uint32_t features = UINT32_C(0x22c2) | EXT4_LARGE_DIR | (pending ? EXT4_NEEDS_RECOVERY : 0);
    uint32_t ro_features = UINT32_C(0x256b) | (pending ? EXT4_ORPHAN_PRESENT : 0);
    bool recovery = (incompat & EXT4_NEEDS_RECOVERY) != 0;
    if (recovery != pending || (incompat & ~features) || (ro & ~ro_features) ||
        (pending && (!(le32(s + 0x5c) & EXT4_HAS_JOURNAL) || !(ro & METADATA_CSUM))))
        return LUME_EXT4_UNSUPPORTED;
    if ((ro & EXT4_ORPHAN_PRESENT) && !(le32(s + 0x5c) & EXT4_ORPHAN_FILE))
        return LUME_EXT4_INVALID;
    uint32_t orphan = le32(s + EXT4_SB_LAST_ORPHAN);
    if (le16(s + 0x3a) != 1 || le32(s + 0x48) != 0 || (!pending && orphan))
        return LUME_EXT4_UNSUPPORTED;
    if ((incompat & CSUM_SEED) && !(ro & METADATA_CSUM))
        return LUME_EXT4_INVALID;
    if ((ro & METADATA_CSUM) &&
        (s[0x175] != 1 || lume_ext4_crc32c(UINT32_MAX, s, 0x3fc) != le32(s + 0x3fc)))
        return LUME_EXT4_INVALID;
    uint32_t shift = le32(s + 0x18);
    if (shift > 2 || le32(s + 0x1c) != shift)
        return LUME_EXT4_UNSUPPORTED;
    uint32_t block_bytes = 1024u << shift;
    uint64_t blocks = le32(s + 4);
    uint64_t reserved_blocks = le32(s + 8), free_blocks = le32(s + 12);
    if (incompat & BIT64)
    {
        blocks |= (uint64_t)le32(s + 0x150) << 32;
        reserved_blocks |= (uint64_t)le32(s + 0x154) << 32;
        free_blocks |= (uint64_t)le32(s + 0x158) << 32;
    }
    uint32_t first = le32(s + 0x14), per_group = le32(s + 0x20);
    uint32_t inodes = le32(s), inode_group = le32(s + 0x28);
    if (orphan && (orphan < le32(s + 0x54) || orphan > inodes))
        return LUME_EXT4_INVALID;
    uint16_t inode_bytes = le16(s + 0x58);
    uint16_t descriptor = (incompat & BIT64) ? le16(s + 0xfe) : 32;
    if (first != (shift == 0) || blocks <= first || blocks > bytes / block_bytes || !per_group ||
        per_group > block_bytes * 8 || !inode_group || inode_group > block_bytes * 8 ||
        inodes < 2 || inode_bytes < 128 || inode_bytes > block_bytes ||
        (inode_bytes & (inode_bytes - 1)) || descriptor < ((incompat & BIT64) ? 64 : 32) ||
        descriptor > block_bytes || (descriptor & (descriptor - 1)) || free_blocks > blocks ||
        reserved_blocks > blocks)
        return LUME_EXT4_INVALID;
    uint64_t groups = (blocks - first - 1) / per_group + 1;
    if (groups > UINT32_MAX || (uint64_t)inodes > groups * inode_group ||
        (uint64_t)(inodes - 1) / inode_group + 1 != groups)
        return LUME_EXT4_INVALID;
    uint64_t table = (uint64_t)(first + 1) * block_bytes;
    if (table >= blocks * block_bytes || groups * descriptor > blocks * block_bytes - table)
        return LUME_EXT4_INVALID;
    *fs = (struct lume_ext4){
        .read = read,
        .context = context,
        .bytes = blocks * block_bytes,
        .blocks = blocks,
        .free_blocks = free_blocks,
        .reserved_blocks = reserved_blocks,
        .filesystem_id = (uint64_t)lume_ext4_crc32c(UINT32_MAX, s + 0x68, 16),
        .block_bytes = block_bytes,
        .inodes = inodes,
        .inodes_per_group = inode_group,
        .groups = (uint32_t)groups,
        .compat = le32(s + 0x5c),
        .incompat = incompat,
        .ro_compat = ro,
        .checksum_seed =
            (incompat & CSUM_SEED) ? le32(s + 0x270) : lume_ext4_crc32c(UINT32_MAX, s + 0x68, 16),
        .dir_hash_seed = {le32(s + 0xec), le32(s + 0xf0), le32(s + 0xf4), le32(s + 0xf8)},
        .dir_hash_flags = le32(s + 0x160),
        .journal_indexed = (le32(s + 0x5c) & EXT4_HAS_JOURNAL) && le32(s + 0xe0) &&
                           s[0xfd] == 1 && le16(s + 0x112),
        .inode_bytes = inode_bytes,
        .descriptor_bytes = descriptor,
        .first_data_block = first};
    return 0;
}

int lume_ext4_open(struct lume_ext4 *fs, lume_ext4_read_bytes read, void *context, uint64_t bytes,
                   void *scratch, size_t scratch_bytes)
{
    return open_filesystem(fs, read, context, bytes, scratch, scratch_bytes, false);
}

int lume_ext4_open_pending(struct lume_ext4 *fs, lume_ext4_read_bytes read, void *context,
                           uint64_t bytes, void *scratch, size_t scratch_bytes)
{
    return open_filesystem(fs, read, context, bytes, scratch, scratch_bytes, true);
}

static struct lume_ext4_time inode_time(const unsigned char *raw, unsigned low, unsigned extra,
                                        unsigned extra_bytes)
{
    uint32_t high = extra_bytes >= extra + 4 - 128 ? le32(raw + extra) : 0;
    int64_t seconds = (int32_t)le32(raw + low) + (int64_t)(high & 3u) * INT64_C(4294967296);
    return (struct lume_ext4_time){.seconds = seconds, .nanoseconds = high >> 2};
}

static int invalid_metadata(const struct lume_ext4 *fs, const char *operation, uint64_t location)
{
    if (fs->metadata_error && !fs->metadata_error->operation)
        *fs->metadata_error = (struct lume_ext4_metadata_error){operation, location};
    return LUME_EXT4_INVALID;
}

int lume_ext4_inode_get(const struct lume_ext4 *fs, uint32_t number, struct lume_ext4_inode *inode,
                        void *scratch, size_t scratch_bytes)
{
    if (!fs || !fs->read || !inode || !scratch || scratch_bytes < fs->block_bytes || !number ||
        number > fs->inodes)
        return LUME_EXT4_INVALID;
    unsigned char *s = scratch;
    uint32_t group = (number - 1) / fs->inodes_per_group;
    uint64_t descriptor = (uint64_t)(fs->first_data_block + 1) * fs->block_bytes +
                          (uint64_t)group * fs->descriptor_bytes;
    int error = read_bytes(fs, descriptor, s, fs->descriptor_bytes);
    if (error)
        return error;
    if (fs->ro_compat & METADATA_CSUM)
    {
        uint16_t expected = le16(s + 0x1e);
        s[0x1e] = s[0x1f] = 0;
        if ((uint16_t)lume_ext4_crc32c(crc_number(fs->checksum_seed, group), s,
                                       fs->descriptor_bytes) != expected)
            return invalid_metadata(fs, "EXT4_META_GROUP_CHECKSUM_OFFSET", descriptor);
    }
    uint64_t table = le32(s + 8);
    if (fs->incompat & BIT64)
        table |= (uint64_t)le32(s + 0x28) << 32;
    uint64_t table_bytes = (uint64_t)fs->inodes_per_group * fs->inode_bytes;
    if (!table || table >= fs->blocks || table_bytes > (fs->blocks - table) * fs->block_bytes)
        return invalid_metadata(fs, "EXT4_META_INODE_TABLE_OFFSET", descriptor);
    uint64_t offset =
        table * fs->block_bytes + (uint64_t)((number - 1) % fs->inodes_per_group) * fs->inode_bytes;
    error = read_bytes(fs, offset, s, fs->inode_bytes);
    if (error)
        return error;
    uint16_t extra = fs->inode_bytes > 128 ? le16(s + 0x80) : 0;
    if ((extra & 3) || extra > fs->inode_bytes - 128)
        return invalid_metadata(fs, "EXT4_META_INODE_SIZE_OFFSET", offset);
    uint32_t generation = le32(s + 0x64);
    if (fs->ro_compat & METADATA_CSUM)
    {
        uint32_t expected = le16(s + 0x7c);
        s[0x7c] = s[0x7d] = 0;
        if (extra >= 4)
        {
            expected |= (uint32_t)le16(s + 0x82) << 16;
            s[0x82] = s[0x83] = 0;
        }
        uint32_t actual = lume_ext4_crc32c(
            crc_number(crc_number(fs->checksum_seed, number), generation), s, fs->inode_bytes);
        if ((extra >= 4 ? actual : (uint16_t)actual) != expected)
            return invalid_metadata(fs, "EXT4_META_INODE_CHECKSUM_OFFSET", offset);
    }
    uint16_t mode = le16(s);
    unsigned type = mode & 0xf000;
    uint32_t flags = le32(s + 0x20);
    if ((type != 0x8000 && type != 0x4000 && type != 0xa000 && type != LUME_EXT4_MODE_FIFO) ||
        (flags & ~(EXTENTS | EXT4_INDEX_INODE)))
        return LUME_EXT4_UNSUPPORTED;
    if (((flags & EXTENTS) && !(fs->incompat & 0x40)) ||
        ((flags & EXT4_INDEX_INODE) && (type != 0x4000 || !(fs->compat & 0x20))) ||
        (type == LUME_EXT4_MODE_FIFO && (flags || le32(s + 4) || le32(s + 0x6c))))
        return invalid_metadata(fs, "EXT4_META_INODE_LAYOUT_OFFSET", offset);
    uint64_t sectors = le32(s + 0x1c);
    if (fs->ro_compat & HUGE_FILE)
        sectors |= (uint64_t)le16(s + 0x74) << 32;
    *inode =
        (struct lume_ext4_inode){.bytes = (uint64_t)le32(s + 4) | (uint64_t)le32(s + 0x6c) << 32,
                                 .sectors = sectors,
                                 .number = number,
                                 .generation = generation,
                                 .flags = flags,
                                 .uid = (uint32_t)le16(s + 2) | (uint32_t)le16(s + 0x78) << 16,
                                 .gid = (uint32_t)le16(s + 0x18) | (uint32_t)le16(s + 0x7a) << 16,
                                 .mode = mode,
                                 .links = le16(s + 0x1a),
                                 .accessed = inode_time(s, 0x8, 0x8c, extra),
                                 .modified = inode_time(s, 0x10, 0x88, extra),
                                 .changed = inode_time(s, 0xc, 0x84, extra)};
    if (inode->bytes > (UINT64_C(1) << 32) * fs->block_bytes)
        return LUME_EXT4_UNSUPPORTED;
    if (!(flags & EXTENTS))
    {
        uint64_t pointers = fs->block_bytes / 4;
        uint64_t maximum = 12 + pointers + pointers * pointers + pointers * pointers * pointers;
        if (inode->bytes > maximum * fs->block_bytes)
            return LUME_EXT4_INVALID;
    }
    for (unsigned i = 0; i != sizeof inode->block; ++i)
        inode->block[i] = s[0x28 + i];
    if (type == 0xa000)
    {
        uint64_t acl = (uint64_t)le32(s + 0x68) | (uint64_t)le16(s + 0x76) << 32;
        uint64_t ea_sectors = acl ? fs->block_bytes / 512 : 0;
        if (!inode->bytes || inode->bytes >= fs->block_bytes || sectors < ea_sectors ||
            (acl && (acl < fs->first_data_block || acl >= fs->blocks)))
            return LUME_EXT4_INVALID;
        if (sectors == ea_sectors)
        {
            if (inode->bytes >= sizeof inode->block || (flags & EXTENTS) ||
                inode->block[inode->bytes])
                return LUME_EXT4_INVALID;
            for (unsigned i = 0; i < inode->bytes; ++i)
                if (!inode->block[i])
                    return LUME_EXT4_INVALID;
            inode->inline_bytes = (uint16_t)inode->bytes;
        }
    }
    return 0;
}

/* A run stops at a mapping boundary or the caller's requested end. */
struct block_run
{
    uint64_t physical, blocks;
};

static int map_indirect(const struct lume_ext4 *fs, const struct lume_ext4_inode *inode,
                        uint32_t logical, uint64_t count, struct block_run *run,
                        unsigned char *scratch)
{
    uint64_t remaining = logical, stride = 1;
    uint32_t pointers = fs->block_bytes / 4;
    unsigned depth = 0, entries = 12, slot = logical;
    const unsigned char *node = inode->block;
    if (logical >= 12)
    {
        remaining -= 12;
        uint64_t capacity = pointers;
        for (depth = 1; depth <= 3; ++depth)
        {
            if (remaining < capacity)
                break;
            remaining -= capacity;
            capacity *= pointers;
        }
        if (depth > 3)
            return LUME_EXT4_INVALID;
        slot = 11 + depth;
        stride = capacity / pointers;
        entries = pointers;
    }
    uint32_t ancestors[3];
    unsigned visited = 0;
    while (depth)
    {
        uint32_t block = le32(node + slot * 4);
        if (!block)
        {
            uint64_t hole = stride * pointers - remaining;
            *run = (struct block_run){0, count < hole ? count : hole};
            return 0;
        }
        for (unsigned i = 0; i < visited; ++i)
            if (block == ancestors[i])
                return LUME_EXT4_INVALID;
        ancestors[visited++] = block;
        int error = read_block(fs, block, scratch);
        if (error)
            return error;
        node = scratch;
        slot = (unsigned)(remaining / stride);
        remaining %= stride;
        stride /= pointers;
        --depth;
    }
    unsigned available = entries - slot;
    if (count < available)
        available = (unsigned)count;
    uint64_t physical = le32(node + slot * 4);
    unsigned length = 0;
    for (; length < available; ++length)
    {
        uint32_t block = le32(node + (slot + length) * 4);
        if (block != (physical ? physical + length : 0))
            break;
        if (block >= fs->blocks)
            return LUME_EXT4_INVALID;
        for (unsigned i = 0; i < visited; ++i)
            if (block == ancestors[i])
                return LUME_EXT4_INVALID;
    }
    *run = (struct block_run){physical, length};
    return 0;
}

struct extent_window
{
    const unsigned char *leaf;
    uint64_t limit;
    unsigned next;
};

static uint64_t extent_disk_block(const unsigned char *entry)
{
    return (uint64_t)le32(entry + 8) | (uint64_t)le16(entry + 6) << 32;
}

static unsigned extent_blocks(unsigned raw)
{
    return raw > 32768 ? raw - 32768 : raw;
}

/* Locate and validate the leaf once for the logical interval it covers. */
static int extent_window(const struct lume_ext4 *fs, const struct lume_ext4_inode *inode,
                         uint32_t logical, struct extent_window *window, unsigned char *scratch)
{
    const unsigned char *node = inode->block;
    size_t bytes = sizeof inode->block;
    unsigned expected_depth = le16(node + 6);
    if (expected_depth > LUME_EXT4_EXTENT_MAX_DEPTH)
        return LUME_EXT4_UNSUPPORTED;
    uint64_t limit = UINT64_C(1) << 32;
    for (;;)
    {
        unsigned entries = le16(node + 2), maximum = le16(node + 4), depth = le16(node + 6);
        if (le16(node) != 0xf30a || depth != expected_depth || entries > maximum || !maximum ||
            maximum > (bytes - 12) / 12 || (depth && !entries))
            return LUME_EXT4_INVALID;
        size_t tail = 12 + maximum * 12;
        if (node == scratch && (fs->ro_compat & METADATA_CSUM) &&
            (tail + 4 > bytes ||
             lume_ext4_crc32c(inode_seed(fs, inode), node, tail) != le32(node + tail)))
            return LUME_EXT4_INVALID;
        uint64_t selected = 0, previous_end = 0, next = limit;
        unsigned slot = 0;
        for (unsigned i = 0; i < entries; ++i)
        {
            const unsigned char *entry = node + 12 + i * 12;
            uint32_t start = le32(entry);
            if (start < previous_end || start >= limit)
                return LUME_EXT4_INVALID;
            if (depth)
            {
                uint64_t child = (uint64_t)le32(entry + 4) | (uint64_t)le16(entry + 8) << 32;
                if (!child || child >= fs->blocks)
                    return LUME_EXT4_INVALID;
                if (start <= logical)
                    selected = child;
                else if (next == limit)
                    next = start;
                previous_end = (uint64_t)start + 1;
            }
            else
            {
                unsigned length = extent_blocks(le16(entry + 4));
                uint64_t block = extent_disk_block(entry);
                previous_end = (uint64_t)start + length;
                if (!length || !block || block >= fs->blocks || length > fs->blocks - block ||
                    previous_end > limit)
                    return LUME_EXT4_INVALID;
                if (previous_end <= logical)
                    slot = i + 1;
            }
        }
        if (!depth || !selected)
        {
            *window = (struct extent_window){depth ? NULL : node, next, slot};
            return 0;
        }
        int error = read_block(fs, selected, scratch);
        if (error)
            return error;
        node = scratch;
        bytes = fs->block_bytes;
        limit = next;
        --expected_depth;
    }
}

static int map_run(const struct lume_ext4 *fs, const struct lume_ext4_inode *inode,
                   uint32_t logical, uint64_t count, struct block_run *run,
                   struct extent_window *window, unsigned char *scratch)
{
    if (!(inode->flags & EXTENTS))
        return map_indirect(fs, inode, logical, count, run, scratch);
    if (logical >= window->limit)
    {
        int error = extent_window(fs, inode, logical, window, scratch);
        if (error)
            return error;
    }
    const unsigned char *node = window->leaf;
    unsigned entries = node ? le16(node + 2) : 0;
    unsigned i = window->next;
    uint64_t physical = 0, end = window->limit;
    if (i < entries)
    {
        const unsigned char *entry = node + 12 + i * 12;
        uint32_t start = le32(entry);
        if (start > logical)
            end = start;
        else
        {
            unsigned raw = le16(entry + 4);
            physical = raw > 32768 ? 0 : extent_disk_block(entry) + logical - start;
            end = (uint64_t)start + extent_blocks(raw);
            ++i;
        }
        for (; i < entries && end - logical < count; ++i)
        {
            entry = node + 12 + i * 12;
            unsigned raw = le16(entry + 4);
            uint64_t block = raw > 32768 ? 0 : extent_disk_block(entry);
            if (le32(entry) != end || block != (physical ? physical + end - logical : 0))
                break;
            end += extent_blocks(raw);
        }
        window->next = i;
    }
    uint64_t available = end - logical;
    *run = (struct block_run){physical, count < available ? count : available};
    return 0;
}

int lume_ext4_read(const struct lume_ext4 *fs, const struct lume_ext4_inode *inode, uint64_t offset,
                   void *out, size_t bytes, void *scratch, size_t scratch_bytes)
{
    if (!fs || !fs->read || !fs->block_bytes || !inode || !scratch ||
        scratch_bytes < fs->block_bytes || (!out && bytes) || offset > inode->bytes ||
        bytes > inode->bytes - offset)
        return LUME_EXT4_INVALID;
    if (!bytes)
        return 0;
    if (inode->inline_bytes)
    {
        if (inode->inline_bytes != inode->bytes || inode->bytes >= sizeof inode->block)
            return LUME_EXT4_INVALID;
        memcpy(out, inode->block + offset, bytes);
        return 0;
    }
    uint64_t last = (offset + bytes - 1) / fs->block_bytes;
    if (last > UINT32_MAX)
        return LUME_EXT4_UNSUPPORTED;
    unsigned char *p = out;
    struct extent_window window = {0};
    while (bytes)
    {
        uint64_t logical = offset / fs->block_bytes;
        struct block_run run;
        int error =
            map_run(fs, inode, (uint32_t)logical, last - logical + 1, &run, &window, scratch);
        if (error)
            return error;
        size_t within = offset % fs->block_bytes;
        uint64_t available = run.blocks * fs->block_bytes - within;
        size_t amount = available < bytes ? (size_t)available : bytes;
        if (run.physical)
        {
            error = read_bytes(fs, run.physical * fs->block_bytes + within, p, amount);
            if (error)
                return error;
        }
        else
            memset(p, 0, amount);
        p += amount;
        bytes -= amount;
        offset += amount;
    }
    return 0;
}

static int read_directory_block(const struct lume_ext4 *fs, const struct lume_ext4_inode *directory,
                                uint32_t logical, unsigned char *data)
{
    struct block_run run;
    struct extent_window window = {0};
    int result = map_run(fs, directory, logical, 1, &run, &window, data);
    if (result)
        return result;
    return run.physical ? read_block(fs, run.physical, data) : LUME_EXT4_INVALID;
}

static int check_dirents(const struct lume_ext4 *fs, const struct lume_ext4_inode *directory,
                         const unsigned char *s, bool index, size_t *end)
{
    *end = fs->block_bytes;
    if (!index && (fs->ro_compat & METADATA_CSUM))
    {
        *end -= 12;
        if (le32(s + *end) || le16(s + *end + 4) != 12 || s[*end + 6] || s[*end + 7] != 0xde ||
            lume_ext4_crc32c(inode_seed(fs, directory), s, *end) != le32(s + *end + 8))
            return invalid_metadata(fs, "EXT4_META_DIRECTORY_TAIL_INODE", directory->number);
    }
    for (size_t at = 0; at < *end;)
    {
        if (*end - at < 8)
            return LUME_EXT4_INVALID;
        uint32_t number = le32(s + at);
        size_t record = le16(s + at + 4);
        unsigned count = (fs->incompat & 2) ? s[at + 6] : le16(s + at + 6);
        if (record < 8 || (record & 3) || record > *end - at || count > 255 || count > record - 8 ||
            number > fs->inodes)
            return LUME_EXT4_INVALID;
        if (number)
        {
            if (!count)
                return LUME_EXT4_INVALID;
            for (unsigned i = 0; i != count; ++i)
                if (!s[at + 8 + i] || s[at + 8 + i] == '/')
                    return LUME_EXT4_INVALID;
        }
        at += record;
    }
    return 0;
}

static int directory_block(const struct lume_ext4 *fs, const struct lume_ext4_inode *directory,
                           uint64_t logical, unsigned char *s, size_t *end)
{
    if (logical > UINT32_MAX)
        return LUME_EXT4_UNSUPPORTED;
    int result = read_directory_block(fs, directory, (uint32_t)logical, s);
    if (result)
        return result;
    bool index =
        (directory->flags & EXT4_INDEX_INODE) && (!logical || lume_ext4_dx_fake_dirent(fs, s));
    /* A classic empty leaf can look like an index block without checksums. */
    if (index && logical && !(fs->ro_compat & METADATA_CSUM) &&
        le16(s + lume_ext4_dx_base((uint32_t)logical)) != lume_ext4_dx_limit(fs, (uint32_t)logical))
        index = false;
    if (index)
    {
        struct lume_ext4_dx dx = {.fs = fs,
                                  .blocks = directory->bytes / fs->block_bytes,
                                  .number = directory->number,
                                  .seed = inode_seed(fs, directory)};
        result = lume_ext4_dx_check(&dx, (uint32_t)logical, s);
        if (result)
            return result;
    }
    return check_dirents(fs, directory, s, index, end);
}

int lume_ext4_readlink(const struct lume_ext4 *fs, const struct lume_ext4_inode *inode, void *out,
                       size_t bytes, void *scratch, size_t scratch_bytes)
{
    if (!fs || !inode || !out || !bytes || !scratch || scratch_bytes < fs->block_bytes ||
        (inode->mode & 0xf000) != 0xa000 || !inode->bytes || inode->bytes >= fs->block_bytes)
        return LUME_EXT4_INVALID;
    int result =
        lume_ext4_read(fs, inode, 0, scratch, (size_t)inode->bytes, scratch, scratch_bytes);
    if (result)
        return result;
    const unsigned char *target = scratch;
    for (size_t i = 0; i < inode->bytes; ++i)
        if (!target[i])
            return LUME_EXT4_INVALID;
    if (bytes > inode->bytes)
        bytes = (size_t)inode->bytes;
    unsigned char *destination = out;
    for (size_t i = 0; i < bytes; ++i)
        destination[i] = target[i];
    return (int)bytes;
}

int lume_ext4_readdir(const struct lume_ext4 *fs, const struct lume_ext4_inode *directory,
                      uint64_t *cursor, struct lume_ext4_entry *entry, void *scratch,
                      size_t scratch_bytes)
{
    if (!fs || !fs->read || !directory || !cursor || !entry || !scratch ||
        scratch_bytes < fs->block_bytes || (directory->mode & 0xf000) != 0x4000 ||
        directory->bytes % fs->block_bytes || *cursor > directory->bytes)
        return LUME_EXT4_INVALID;
    unsigned char *s = scratch;
    while (*cursor < directory->bytes)
    {
        uint64_t logical = *cursor / fs->block_bytes;
        size_t within = *cursor % fs->block_bytes, end;
        int error = directory_block(fs, directory, logical, s, &end);
        if (error)
            return error;
        if (within > end)
            return LUME_EXT4_INVALID;
        for (size_t at = 0; at < end;)
        {
            size_t record = le16(s + at + 4);
            if (at < within && at + record > within)
                return LUME_EXT4_INVALID;
            uint32_t number = le32(s + at);
            if (at >= within && number)
            {
                unsigned count = (fs->incompat & 2) ? s[at + 6] : le16(s + at + 6);
                entry->inode = number;
                for (unsigned i = 0; i != count; ++i)
                    entry->name[i] = (char)s[at + 8 + i];
                entry->name[count] = 0;
                *cursor = logical * fs->block_bytes + at + record;
                return 1;
            }
            at += record;
        }
        *cursor = (logical + 1) * fs->block_bytes;
    }
    return 0;
}

static uint32_t dirent_number(const struct lume_ext4 *fs, const unsigned char *data, size_t end,
                              const char *name, size_t length)
{
    for (size_t at = 0; at < end; at += le16(data + at + 4))
    {
        const unsigned char *entry = data + at;
        unsigned count = fs->incompat & 2 ? entry[6] : le16(entry + 6);
        uint32_t number = le32(entry);
        if (number && count == length && !memcmp(entry + 8, name, length))
            return number;
    }
    return 0;
}

struct read_index
{
    const struct lume_ext4 *fs;
    const struct lume_ext4_inode *inode;
    unsigned char *data;
};

static int read_index_block(void *context, uint32_t block, unsigned char **data)
{
    struct read_index *reader = context;
    *data = reader->data;
    return read_directory_block(reader->fs, reader->inode, block, *data);
}

static int indexed_lookup(const struct lume_ext4 *fs, const struct lume_ext4_inode *directory,
                          const char *name, size_t length, unsigned char *data, uint32_t *number)
{
    struct read_index reader = {fs, directory, data};
    struct lume_ext4_dx dx = {.fs = fs,
                              .context = &reader,
                              .read = read_index_block,
                              .blocks = directory->bytes / fs->block_bytes,
                              .number = directory->number,
                              .seed = inode_seed(fs, directory)};
    struct lume_ext4_dx_path path;
    int result = lume_ext4_dx_probe(&dx, name, length, &path);
    if (result)
        return result;
    do
    {
        result = read_directory_block(fs, directory, path.leaf, data);
        if (result)
            return result;
        if (lume_ext4_dx_fake_dirent(fs, data) &&
            le16(data + lume_ext4_dx_base(path.leaf)) == lume_ext4_dx_limit(fs, path.leaf))
            return LUME_EXT4_INVALID;
        size_t end;
        result = check_dirents(fs, directory, data, false, &end);
        if (result)
            return result;
        *number = dirent_number(fs, data, end, name, length);
        if (*number)
            return 0;
        result = lume_ext4_dx_next(&dx, &path);
    } while (result > 0);
    return result ? result : LUME_EXT4_NOT_FOUND;
}

int lume_ext4_lookup(const struct lume_ext4 *fs, const struct lume_ext4_inode *directory,
                     const char *name, struct lume_ext4_inode *out, void *scratch,
                     size_t scratch_bytes)
{
    if (!fs || !fs->read || !directory || !name || !out || !scratch ||
        scratch_bytes < fs->block_bytes || (directory->mode & 0xf000) != 0x4000 ||
        directory->bytes % fs->block_bytes)
        return LUME_EXT4_INVALID;
    size_t length = 0;
    while (name[length])
    {
        if (name[length] == '/' || length == 255)
            return LUME_EXT4_INVALID;
        ++length;
    }
    if (!length)
        return LUME_EXT4_INVALID;
    unsigned char *s = scratch;
    bool dot = name[0] == '.' && (length == 1 || (length == 2 && name[1] == '.'));
    if ((directory->flags & EXT4_INDEX_INODE) && !dot)
    {
        uint32_t number;
        int result = indexed_lookup(fs, directory, name, length, s, &number);
        if (result != LUME_EXT4_UNSUPPORTED)
            return result ? result : lume_ext4_inode_get(fs, number, out, scratch, scratch_bytes);
    }
    uint64_t blocks =
        dot && (directory->flags & EXT4_INDEX_INODE) ? 1 : directory->bytes / fs->block_bytes;
    for (uint64_t logical = 0; logical < blocks; ++logical)
    {
        size_t end;
        int result = directory_block(fs, directory, logical, s, &end);
        if (result)
            return result;
        uint32_t number = dirent_number(fs, s, end, name, length);
        if (number)
            return lume_ext4_inode_get(fs, number, out, scratch, scratch_bytes);
    }
    return LUME_EXT4_NOT_FOUND;
}
