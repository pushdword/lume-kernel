#include "lume/usb.h"

bool lume_usb_configuration_inputs(const uint8_t *p, size_t bytes,
                                   struct lume_usb_configuration *out)
{
    *out = (struct lume_usb_configuration){0};
    if (bytes < 9 || p[0] != 9 || p[1] != 2 || !p[5] || !p[4])
        return false;
    unsigned total = p[2] | ((unsigned)p[3] << 8);
    if (total != bytes)
        return false;
    out->value = p[5];
    unsigned input = 0;
    uint8_t interface = 0;
    bool selected = false;
    uint32_t endpoints = 0;
    for (size_t offset = 9; offset < bytes;)
    {
        const uint8_t *d = p + offset;
        if (bytes - offset < 2 || d[0] < 2 || d[0] > bytes - offset)
            return false;
        if (d[1] == 4)
        {
            if (d[0] < 9)
                return false;
            interface = d[2];
            selected = false;
            input = !d[3] && d[5] == 3 && d[6] == 1 && (d[7] == 1 || d[7] == 2) ? d[7] : 0;
            if (!d[3] && d[5] == 9)
                out->hub = true;
        }
        else if (d[1] == 5)
        {
            if (d[0] < 7)
                return false;
            unsigned packet = (d[4] | ((unsigned)d[5] << 8));
            if (input && !selected && (d[2] & 0x80) && (d[2] & 15) && !(d[2] & 0x70) &&
                (d[3] & 3) == 3 && packet >= (input == 1 ? 8u : 3u) && packet <= 64 && d[6])
            {
                unsigned endpoint = d[2] & 15;
                if (out->inputs == LUME_USB_INPUTS || (endpoints & (1u << endpoint)))
                    return false;
                for (unsigned n = 0; n < out->inputs; ++n)
                    if (out->input[n].interface == interface)
                        return false;
                endpoints |= 1u << endpoint;
                out->input[out->inputs++] = (struct lume_usb_interface){.configuration = out->value,
                                                                        .keyboard = input == 1,
                                                                        .mouse = input == 2,
                                                                        .interface = interface,
                                                                        .endpoint = d[2],
                                                                        .packet = (uint16_t)packet,
                                                                        .interval = d[6]};
                selected = true;
            }
        }
        offset += d[0];
    }
    return true;
}

bool lume_usb_configuration(const uint8_t *p, size_t bytes, struct lume_usb_interface *out)
{
    struct lume_usb_configuration configuration;
    *out = (struct lume_usb_interface){0};
    if (!lume_usb_configuration_inputs(p, bytes, &configuration))
        return false;
    if (configuration.inputs)
        *out = configuration.input[0];
    out->configuration = configuration.value;
    out->hub = configuration.hub;
    return true;
}
