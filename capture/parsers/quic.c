/* Copyright 2012-2016 AOL Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this Software except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * https://www.chromium.org/quic
 * https://docs.google.com/document/d/1WJvyZflAO2pq77yOLbp9NsGjC1CHetAXV8I0fQe-B_U
 *
 */
#include "moloch.h"
#include <arpa/inet.h>

extern MolochConfig_t        config;
static int hostField;
static int uaField;
static int versionField;

/******************************************************************************/
int quic_udp_parser(MolochSession_t *session, void *UNUSED(uw), const unsigned char *data, int len, int UNUSED(which))
{
    int version = -1;
    int offset = 1;

    // PUBLIC_FLAG_RESET
    if (data[0] & 0x02) {
        return 0;
    }

    // CID
    if (data[0] & 0x08) {
        offset += 8;
    }

    // Get version
    if (data[0] & 0x01 && data[offset] == 'Q') {
        version = (data[offset+1] - '0') * 100 +
                  (data[offset+2] - '0') * 10 +
                  (data[offset+3] - '0');
        offset += 4;
    }

    // Unsupported version
    if (version < 24) {
        moloch_parsers_unregister(session, uw);
        return 0;
    }

    // Diversification nonce Q033 and later
    if ((data[0] & 0x04) && version >= 33) {
        offset += 32;
    }

    // Packet number size
    if ((data[0] & 0x30) == 0) {
        offset++;
    } else {
        offset += ((data[0] & 0x30) >> 4) * 2;
    }

    // Hash
    offset += 12;

    // Private Flags
    offset++;

    if (offset > len)
        return 0;

    BSB bsb;
    BSB_INIT(bsb, data+offset, len-offset);

    while (!BSB_IS_ERROR(bsb) && BSB_REMAINING(bsb)) {
        uint8_t type = 0;
        BSB_LIMPORT_u08(bsb, type);

        //1fdooossB
        if ((type & 0x80) == 0) {
            return 0;
        }

        int offsetLen = 0;
        if (type & 0x1C) {
            offsetLen = ((type & 0x1C) >> 2) + 1;
        }

        int streamLen = (type & 0x03) + 1;

        BSB_LIMPORT_skip(bsb, streamLen + offsetLen);

        int dataLen = BSB_REMAINING(bsb);
        if (type & 0x20) {
            BSB_LIMPORT_u16(bsb, dataLen);
        }

        if (BSB_IS_ERROR(bsb))
            return 0;

        BSB dbsb;
        BSB_INIT(dbsb, BSB_WORK_PTR(bsb), MIN(dataLen, BSB_REMAINING(bsb)));
        BSB_IMPORT_skip(bsb, dataLen);

        guchar *tag = 0;
        int     tagLen = 0;
        BSB_LIMPORT_ptr(dbsb, tag, 4);
        BSB_LIMPORT_u16(dbsb, tagLen);
        BSB_LIMPORT_skip(dbsb, 2);

        if (tag && memcmp(tag, "CHLO", 4) == 0) {
            moloch_session_add_protocol(session, "quic");
        } else {
            return 0;
        }

        guchar *tagDataStart = dbsb.buf + tagLen*8 + 8;

        int start = 0;
        while (!BSB_IS_ERROR(dbsb) && BSB_REMAINING(dbsb) && tagLen > 0) {
            guchar *subTag = 0;
            int     endOffset = 0;

            BSB_LIMPORT_ptr(dbsb, subTag, 4);
            BSB_LIMPORT_u32(dbsb, endOffset);

            if (!subTag)
                return 0;

            if (memcmp(subTag, "SNI\x00", 4) == 0) {
                moloch_field_string_add(hostField, session, (char *)tagDataStart+start, endOffset-start, TRUE);
            } else if (memcmp(subTag, "UAID", 4) == 0) {
                moloch_field_string_add(uaField, session, (char *)tagDataStart+start, endOffset-start, TRUE);
            } else if (memcmp(subTag, "VER\x00", 4) == 0) {
                moloch_field_string_add(versionField, session, (char *)tagDataStart+start, endOffset-start, TRUE);
            } else {
                //LOG("Subtag: %4.4s len: %d %.*s", subTag, endOffset-start, endOffset-start, tagDataStart+start);
            }
            start = endOffset;
            tagLen--;
        }
    }

    return 0;
}
/******************************************************************************/
void quic_udp_classify(MolochSession_t *session, const unsigned char *data, int len, int UNUSED(which), void *UNUSED(uw))
{
    if (len > 100 && (data[0] & 0x83) == 0x01) {
        moloch_parsers_register(session, quic_udp_parser, 0, 0);
    }
}
/******************************************************************************/
void moloch_parser_init()
{
    moloch_parsers_classifier_register_udp("quic", NULL, 9, (const unsigned char *)"Q03", 3, quic_udp_classify);
    moloch_parsers_classifier_register_udp("quic", NULL, 9, (const unsigned char *)"Q02", 3, quic_udp_classify);

    hostField = moloch_field_define("quic", "lotermfield",
        "host.quic", "Hostname", "quic.host-term", 
        "QUIC host header field", 
        MOLOCH_FIELD_TYPE_STR_HASH,  MOLOCH_FIELD_FLAG_CNT, 
        "aliases", "[\"quic.host\"]", NULL);

    uaField = moloch_field_define("quic", "termfield",
        "quic.user-agent", "User-Agent", "quic.ua-term",
        "User-Agent",
        MOLOCH_FIELD_TYPE_STR_HASH,  MOLOCH_FIELD_FLAG_CNT,
        NULL);

    versionField = moloch_field_define("quic", "termfield",
        "quic.version", "Version", "quic.version-term",
        "QUIC Version",
        MOLOCH_FIELD_TYPE_STR_HASH,  MOLOCH_FIELD_FLAG_CNT,
        NULL);
}
