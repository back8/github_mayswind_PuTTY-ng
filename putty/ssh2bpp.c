/*
 * Binary packet protocol for SSH-2.
 */

#include <assert.h>

#include "putty.h"
#include "ssh.h"
#include "sshbpp.h"
#include "sshcr.h"

struct ssh2_bpp_direction {
    unsigned long sequence;
    ssh2_cipher *cipher;
    ssh2_mac *mac;
    int etm_mode;
};

struct ssh2_bpp_state {
    int crState;
    long len, pad, payload, packetlen, maclen, length, maxlen;
    unsigned char *buf;
    size_t bufsize;
    unsigned char *data;
    unsigned cipherblk;
    PktIn *pktin;
    struct DataTransferStats *stats;
    int cbc_ignore_workaround;

    struct ssh2_bpp_direction in, out;
    /* comp and decomp logically belong in the per-direction
     * substructure, except that they have different types */
    ssh_decompressor *in_decomp;
    ssh_compressor *out_comp;

    int pending_newkeys;

    BinaryPacketProtocol bpp;
};

static void ssh2_bpp_free(BinaryPacketProtocol *bpp);
static void ssh2_bpp_handle_input(BinaryPacketProtocol *bpp);
static void ssh2_bpp_handle_output(BinaryPacketProtocol *bpp);
static PktOut *ssh2_bpp_new_pktout(int type);

static const struct BinaryPacketProtocolVtable ssh2_bpp_vtable = {
    ssh2_bpp_free,
    ssh2_bpp_handle_input,
    ssh2_bpp_handle_output,
    ssh2_bpp_new_pktout,
    ssh2_bpp_queue_disconnect, /* in sshcommon.c */
};

BinaryPacketProtocol *ssh2_bpp_new(struct DataTransferStats *stats)
{
    struct ssh2_bpp_state *s = snew(struct ssh2_bpp_state);
    memset(s, 0, sizeof(*s));
    s->bpp.vt = &ssh2_bpp_vtable;
    s->stats = stats;
    ssh_bpp_common_setup(&s->bpp);
    return &s->bpp;
}

static void ssh2_bpp_free(BinaryPacketProtocol *bpp)
{
    struct ssh2_bpp_state *s = FROMFIELD(bpp, struct ssh2_bpp_state, bpp);
    sfree(s->buf);
    if (s->out.cipher)
        ssh2_cipher_free(s->out.cipher);
    if (s->out.mac)
        ssh2_mac_free(s->out.mac);
    if (s->out_comp)
        ssh_compressor_free(s->out_comp);
    if (s->in.cipher)
        ssh2_cipher_free(s->in.cipher);
    if (s->in.mac)
        ssh2_mac_free(s->in.mac);
    if (s->in_decomp)
        ssh_decompressor_free(s->in_decomp);
    sfree(s->pktin);
    sfree(s);
}

void ssh2_bpp_new_outgoing_crypto(
    BinaryPacketProtocol *bpp,
    const struct ssh2_cipheralg *cipher, const void *ckey, const void *iv,
    const struct ssh2_macalg *mac, int etm_mode, const void *mac_key,
    const struct ssh_compression_alg *compression)
{
    struct ssh2_bpp_state *s;
    assert(bpp->vt == &ssh2_bpp_vtable);
    s = FROMFIELD(bpp, struct ssh2_bpp_state, bpp);

    if (s->out.cipher)
        ssh2_cipher_free(s->out.cipher);
    if (s->out.mac)
        ssh2_mac_free(s->out.mac);
    if (s->out_comp)
        ssh_compressor_free(s->out_comp);

    if (cipher) {
        s->out.cipher = ssh2_cipher_new(cipher);
        ssh2_cipher_setkey(s->out.cipher, ckey);
        ssh2_cipher_setiv(s->out.cipher, iv);

        s->cbc_ignore_workaround = (
            (ssh2_cipher_alg(s->out.cipher)->flags & SSH_CIPHER_IS_CBC) &&
            !(s->bpp.remote_bugs & BUG_CHOKES_ON_SSH2_IGNORE));
    } else {
        s->out.cipher = NULL;
        s->cbc_ignore_workaround = FALSE;
    }
    s->out.etm_mode = etm_mode;
    if (mac) {
        s->out.mac = ssh2_mac_new(mac, s->out.cipher);
        mac->setkey(s->out.mac, mac_key);
    } else {
        s->out.mac = NULL;
    }

    /* 'compression' is always non-NULL, because no compression is
     * indicated by ssh_comp_none. But this setup call may return a
     * null out_comp. */
    s->out_comp = ssh_compressor_new(compression);
}

void ssh2_bpp_new_incoming_crypto(
    BinaryPacketProtocol *bpp,
    const struct ssh2_cipheralg *cipher, const void *ckey, const void *iv,
    const struct ssh2_macalg *mac, int etm_mode, const void *mac_key,
    const struct ssh_compression_alg *compression)
{
    struct ssh2_bpp_state *s;
    assert(bpp->vt == &ssh2_bpp_vtable);
    s = FROMFIELD(bpp, struct ssh2_bpp_state, bpp);

    if (s->in.cipher)
        ssh2_cipher_free(s->in.cipher);
    if (s->in.mac)
        ssh2_mac_free(s->in.mac);
    if (s->in_decomp)
        ssh_decompressor_free(s->in_decomp);

    if (cipher) {
        s->in.cipher = ssh2_cipher_new(cipher);
        ssh2_cipher_setkey(s->in.cipher, ckey);
        ssh2_cipher_setiv(s->in.cipher, iv);
    } else {
        s->in.cipher = NULL;
    }
    s->in.etm_mode = etm_mode;
    if (mac) {
        s->in.mac = ssh2_mac_new(mac, s->in.cipher);
        mac->setkey(s->in.mac, mac_key);
    } else {
        s->in.mac = NULL;
    }

    /* 'compression' is always non-NULL, because no compression is
     * indicated by ssh_comp_none. But this setup call may return a
     * null in_decomp. */
    s->in_decomp = ssh_decompressor_new(compression);

    /* Clear the pending_newkeys flag, so that handle_input below will
     * start consuming the input data again. */
    s->pending_newkeys = FALSE;
}

#define BPP_READ(ptr, len) do                                   \
    {                                                           \
        crMaybeWaitUntilV(s->bpp.input_eof ||                   \
                          bufchain_try_fetch_consume(           \
                              s->bpp.in_raw, ptr, len));        \
        if (s->bpp.input_eof)                                   \
            goto eof;                                           \
    } while (0)

static void ssh2_bpp_handle_input(BinaryPacketProtocol *bpp)
{
    struct ssh2_bpp_state *s = FROMFIELD(bpp, struct ssh2_bpp_state, bpp);

    crBegin(s->crState);

    while (1) {
        s->maxlen = 0;
        s->length = 0;
        if (s->in.cipher)
            s->cipherblk = ssh2_cipher_alg(s->in.cipher)->blksize;
        else
            s->cipherblk = 8;
        if (s->cipherblk < 8)
            s->cipherblk = 8;
        s->maclen = s->in.mac ? ssh2_mac_alg(s->in.mac)->len : 0;

        if (s->in.cipher &&
            (ssh2_cipher_alg(s->in.cipher)->flags & SSH_CIPHER_IS_CBC) &&
            s->in.mac && !s->in.etm_mode) {
            /*
             * When dealing with a CBC-mode cipher, we want to avoid the
             * possibility of an attacker's tweaking the ciphertext stream
             * so as to cause us to feed the same block to the block
             * cipher more than once and thus leak information
             * (VU#958563).  The way we do this is not to take any
             * decisions on the basis of anything we've decrypted until
             * we've verified it with a MAC.  That includes the packet
             * length, so we just read data and check the MAC repeatedly,
             * and when the MAC passes, see if the length we've got is
             * plausible.
             *
             * This defence is unnecessary in OpenSSH ETM mode, because
             * the whole point of ETM mode is that the attacker can't
             * tweak the ciphertext stream at all without the MAC
             * detecting it before we decrypt anything.
             */

            /*
             * Make sure we have buffer space for a maximum-size packet.
             */
            unsigned buflimit;
            buflimit = OUR_V2_PACKETLIMIT + s->maclen;
            if (s->bufsize < buflimit) {
                s->bufsize = buflimit;
                s->buf = sresize(s->buf, s->bufsize, unsigned char);
            }

            /* Read an amount corresponding to the MAC. */
            BPP_READ(s->buf, s->maclen);

            s->packetlen = 0;
            ssh2_mac_start(s->in.mac);
            put_uint32(s->in.mac, s->in.sequence);

            for (;;) { /* Once around this loop per cipher block. */
                /* Read another cipher-block's worth, and tack it on to
                 * the end. */
                BPP_READ(s->buf + (s->packetlen + s->maclen), s->cipherblk);
                /* Decrypt one more block (a little further back in
                 * the stream). */
                ssh2_cipher_decrypt(s->in.cipher,
                                    s->buf + s->packetlen, s->cipherblk);

                /* Feed that block to the MAC. */
                marshal_put_data(s->in.mac,
                         s->buf + s->packetlen, s->cipherblk);
                s->packetlen += s->cipherblk;

                /* See if that gives us a valid packet. */
                if (ssh2_mac_verresult(s->in.mac, s->buf + s->packetlen) &&
                    ((s->len = toint(GET_32BIT(s->buf))) ==
                     s->packetlen-4))
                    break;
                if (s->packetlen >= (long)OUR_V2_PACKETLIMIT) {
                    ssh_sw_abort(s->bpp.ssh,
                                 "No valid incoming packet found");
                    crStopV;
                }
            }
            s->maxlen = s->packetlen + s->maclen;

            /*
             * Now transfer the data into an output packet.
             */
            s->pktin = snew_plus(PktIn, s->maxlen);
            s->pktin->qnode.prev = s->pktin->qnode.next = NULL;
            s->pktin->type = 0;
            s->pktin->qnode.on_free_queue = FALSE;
            s->data = (unsigned char *)snew_plus_get_aux(s->pktin);
            memcpy(s->data, s->buf, s->maxlen);
        } else if (s->in.mac && s->in.etm_mode) {
            if (s->bufsize < 4) {
                s->bufsize = 4;
                s->buf = sresize(s->buf, s->bufsize, unsigned char);
            }

            /*
             * OpenSSH encrypt-then-MAC mode: the packet length is
             * unencrypted, unless the cipher supports length encryption.
             */
            BPP_READ(s->buf, 4);

            /* Cipher supports length decryption, so do it */
            if (s->in.cipher && (ssh2_cipher_alg(s->in.cipher)->flags &
                                 SSH_CIPHER_SEPARATE_LENGTH)) {
                /* Keep the packet the same though, so the MAC passes */
                unsigned char len[4];
                memcpy(len, s->buf, 4);
                ssh2_cipher_decrypt_length(
                    s->in.cipher, len, 4, s->in.sequence);
                s->len = toint(GET_32BIT(len));
            } else {
                s->len = toint(GET_32BIT(s->buf));
            }

            /*
             * _Completely_ silly lengths should be stomped on before they
             * do us any more damage.
             */
            if (s->len < 0 || s->len > (long)OUR_V2_PACKETLIMIT ||
                s->len % s->cipherblk != 0) {
                ssh_sw_abort(s->bpp.ssh,
                             "Incoming packet length field was garbled");
                crStopV;
            }

            /*
             * So now we can work out the total packet length.
             */
            s->packetlen = s->len + 4;

            /*
             * Allocate the packet to return, now we know its length.
             */
            s->pktin = snew_plus(PktIn, OUR_V2_PACKETLIMIT + s->maclen);
            s->pktin->qnode.prev = s->pktin->qnode.next = NULL;
            s->pktin->type = 0;
            s->pktin->qnode.on_free_queue = FALSE;
            s->data = (unsigned char *)snew_plus_get_aux(s->pktin);
            memcpy(s->data, s->buf, 4);

            /*
             * Read the remainder of the packet.
             */
            BPP_READ(s->data + 4, s->packetlen + s->maclen - 4);

            /*
             * Check the MAC.
             */
            if (s->in.mac && !ssh2_mac_verify(
                    s->in.mac, s->data, s->len + 4, s->in.sequence)) {
                ssh_sw_abort(s->bpp.ssh, "Incorrect MAC received on packet");
                crStopV;
            }

            /* Decrypt everything between the length field and the MAC. */
            if (s->in.cipher)
                ssh2_cipher_decrypt(
                    s->in.cipher, s->data + 4, s->packetlen - 4);
        } else {
            if (s->bufsize < s->cipherblk) {
                s->bufsize = s->cipherblk;
                s->buf = sresize(s->buf, s->bufsize, unsigned char);
            }

            /*
             * Acquire and decrypt the first block of the packet. This will
             * contain the length and padding details.
             */
            BPP_READ(s->buf, s->cipherblk);

            if (s->in.cipher)
                ssh2_cipher_decrypt(
                    s->in.cipher, s->buf, s->cipherblk);

            /*
             * Now get the length figure.
             */
            s->len = toint(GET_32BIT(s->buf));

            /*
             * _Completely_ silly lengths should be stomped on before they
             * do us any more damage.
             */
            if (s->len < 0 || s->len > (long)OUR_V2_PACKETLIMIT ||
                (s->len + 4) % s->cipherblk != 0) {
                ssh_sw_abort(s->bpp.ssh,
                             "Incoming packet was garbled on decryption");
                crStopV;
            }

            /*
             * So now we can work out the total packet length.
             */
            s->packetlen = s->len + 4;

            /*
             * Allocate the packet to return, now we know its length.
             */
            s->maxlen = s->packetlen + s->maclen;
            s->pktin = snew_plus(PktIn, s->maxlen);
            s->pktin->qnode.prev = s->pktin->qnode.next = NULL;
            s->pktin->type = 0;
            s->pktin->qnode.on_free_queue = FALSE;
            s->data = (unsigned char *)snew_plus_get_aux(s->pktin);
            memcpy(s->data, s->buf, s->cipherblk);

            /*
             * Read and decrypt the remainder of the packet.
             */
            BPP_READ(s->data + s->cipherblk,
                     s->packetlen + s->maclen - s->cipherblk);

            /* Decrypt everything _except_ the MAC. */
            if (s->in.cipher)
                ssh2_cipher_decrypt(
                    s->in.cipher,
                    s->data + s->cipherblk, s->packetlen - s->cipherblk);

            /*
             * Check the MAC.
             */
            if (s->in.mac && !ssh2_mac_verify(
                    s->in.mac, s->data, s->len + 4, s->in.sequence)) {
                ssh_sw_abort(s->bpp.ssh, "Incorrect MAC received on packet");
                crStopV;
            }
        }
        /* Get and sanity-check the amount of random padding. */
        s->pad = s->data[4];
        if (s->pad < 4 || s->len - s->pad < 1) {
            ssh_sw_abort(s->bpp.ssh,
                         "Invalid padding length on received packet");
            crStopV;
        }
        /*
         * This enables us to deduce the payload length.
         */
        s->payload = s->len - s->pad - 1;

        s->length = s->payload + 5;

        DTS_CONSUME(s->stats, in, s->packetlen);

        s->pktin->sequence = s->in.sequence++;

        s->length = s->packetlen - s->pad;
        assert(s->length >= 0);

        /*
         * Decompress packet payload.
         */
        {
            unsigned char *newpayload;
            int newlen;
            if (s->in_decomp && ssh_decompressor_decompress(
                    s->in_decomp, s->data + 5, s->length - 5,
                    &newpayload, &newlen)) {
                if (s->maxlen < newlen + 5) {
                    PktIn *old_pktin = s->pktin;

                    s->maxlen = newlen + 5;
                    s->pktin = snew_plus(PktIn, s->maxlen);
                    *s->pktin = *old_pktin; /* structure copy */
                    s->data = (unsigned char *)snew_plus_get_aux(s->pktin);

                    smemclr(old_pktin, s->packetlen + s->maclen);
                    sfree(old_pktin);
                }
                s->length = 5 + newlen;
                memcpy(s->data + 5, newpayload, newlen);
                sfree(newpayload);
            }
        }

        /*
         * Now we can identify the semantic content of the packet,
         * and also the initial type byte.
         */
        if (s->length <= 5) { /* == 5 we hope, but robustness */
            /*
             * RFC 4253 doesn't explicitly say that completely empty
             * packets with no type byte are forbidden. We handle them
             * here by giving them a type code larger than 0xFF, which
             * will be picked up at the next layer and trigger
             * SSH_MSG_UNIMPLEMENTED.
             */
            s->pktin->type = SSH_MSG_NO_TYPE_CODE;
            s->data += 5;
            s->length = 0;
        } else {
            s->pktin->type = s->data[5];
            s->data += 6;
            s->length -= 6;
        }
        BinarySource_INIT(s->pktin, s->data, s->length);

        if (s->bpp.logctx) {
            logblank_t blanks[MAX_BLANKS];
            int nblanks = ssh2_censor_packet(
                s->bpp.pls, s->pktin->type, FALSE,
                make_ptrlen(s->data, s->length), blanks);
            log_packet(s->bpp.logctx, PKT_INCOMING, s->pktin->type,
                       ssh2_pkt_type(s->bpp.pls->kctx, s->bpp.pls->actx,
                                     s->pktin->type),
                       s->data, s->length, nblanks, blanks,
                       &s->pktin->sequence, 0, NULL);
        }

        if (ssh2_bpp_check_unimplemented(&s->bpp, s->pktin)) {
            sfree(s->pktin);
            s->pktin = NULL;
            continue;
        }

        pq_push(&s->bpp.in_pq, s->pktin);

        {
            int type;
            type = s->pktin->type;
            s->pktin = NULL;

            if (type == SSH2_MSG_NEWKEYS) {
                /*
                 * Mild layer violation: in this situation we must
                 * suspend processing of the input byte stream until
                 * the transport layer has initialised the new keys by
                 * calling ssh2_bpp_new_incoming_crypto above.
                 */
                s->pending_newkeys = TRUE;
                crWaitUntilV(!s->pending_newkeys);
            }
        }
    }

  eof:
    if (!s->bpp.expect_close) {
        ssh_remote_error(s->bpp.ssh,
                         "Server unexpectedly closed network connection");
    } else {
        ssh_remote_eof(s->bpp.ssh, "Server closed network connection");
    }
    return;  /* avoid touching s now it's been freed */

    crFinishV;
}

static PktOut *ssh2_bpp_new_pktout(int pkt_type)
{
    PktOut *pkt = ssh_new_packet();
    pkt->length = 5; /* space for packet length + padding length */
    pkt->minlen = 0;
    pkt->type = pkt_type;
    put_byte(pkt, pkt_type);
    pkt->prefix = pkt->length;
    return pkt;
}

static void ssh2_bpp_format_packet_inner(struct ssh2_bpp_state *s, PktOut *pkt)
{
    int origlen, cipherblk, maclen, padding, unencrypted_prefix, i;

    if (s->bpp.logctx) {
        ptrlen pktdata = make_ptrlen(pkt->data + pkt->prefix,
                                     pkt->length - pkt->prefix);
        logblank_t blanks[MAX_BLANKS];
        int nblanks = ssh2_censor_packet(
            s->bpp.pls, pkt->type, TRUE, pktdata, blanks);
        log_packet(s->bpp.logctx, PKT_OUTGOING, pkt->type,
                   ssh2_pkt_type(s->bpp.pls->kctx, s->bpp.pls->actx,
                                 pkt->type),
                   pktdata.ptr, pktdata.len, nblanks, blanks, &s->out.sequence,
                   pkt->downstream_id, pkt->additional_log_text);
    }

    cipherblk = s->out.cipher ? ssh2_cipher_alg(s->out.cipher)->blksize : 8;
    cipherblk = cipherblk < 8 ? 8 : cipherblk;  /* or 8 if blksize < 8 */

    if (s->out_comp) {
        unsigned char *newpayload;
        int minlen, newlen;

        /*
         * Compress packet payload.
         */
        minlen = pkt->minlen;
        if (minlen) {
            /*
             * Work out how much compressed data we need (at least) to
             * make the overall packet length come to pkt->minlen.
             */
            if (s->out.mac)
                minlen -= ssh2_mac_alg(s->out.mac)->len;
            minlen -= 8;              /* length field + min padding */
        }

        ssh_compressor_compress(s->out_comp, pkt->data + 5, pkt->length - 5,
                                &newpayload, &newlen, minlen);
        pkt->length = 5;
        marshal_put_data(pkt, newpayload, newlen);
        sfree(newpayload);
    }

    /*
     * Add padding. At least four bytes, and must also bring total
     * length (minus MAC) up to a multiple of the block size.
     * If pkt->forcepad is set, make sure the packet is at least that size
     * after padding.
     */
    padding = 4;
    unencrypted_prefix = (s->out.mac && s->out.etm_mode) ? 4 : 0;
    padding +=
        (cipherblk - (pkt->length - unencrypted_prefix + padding) % cipherblk)
        % cipherblk;
    assert(padding <= 255);
    maclen = s->out.mac ? ssh2_mac_alg(s->out.mac)->len : 0;
    origlen = pkt->length;
    for (i = 0; i < padding; i++)
        put_byte(pkt, random_byte());
    pkt->data[4] = padding;
    PUT_32BIT(pkt->data, origlen + padding - 4);

    /* Encrypt length if the scheme requires it */
    if (s->out.cipher &&
        (ssh2_cipher_alg(s->out.cipher)->flags & SSH_CIPHER_SEPARATE_LENGTH)) {
        ssh2_cipher_encrypt_length(s->out.cipher, pkt->data, 4,
                                   s->out.sequence);
    }

    put_padding(pkt, maclen, 0);

    if (s->out.mac && s->out.etm_mode) {
        /*
         * OpenSSH-defined encrypt-then-MAC protocol.
         */
        if (s->out.cipher)
            ssh2_cipher_encrypt(s->out.cipher,
                                pkt->data + 4, origlen + padding - 4);
        ssh2_mac_generate(s->out.mac, pkt->data, origlen + padding,
                          s->out.sequence);
    } else {
        /*
         * SSH-2 standard protocol.
         */
        if (s->out.mac)
            ssh2_mac_generate(s->out.mac, pkt->data, origlen + padding,
                              s->out.sequence);
        if (s->out.cipher)
            ssh2_cipher_encrypt(s->out.cipher, pkt->data, origlen + padding);
    }

    s->out.sequence++;       /* whether or not we MACed */

    DTS_CONSUME(s->stats, out, origlen + padding);

}

static void ssh2_bpp_format_packet(struct ssh2_bpp_state *s, PktOut *pkt)
{
    if (pkt->minlen > 0 && !s->out_comp) {
        /*
         * If we've been told to pad the packet out to a given minimum
         * length, but we're not compressing (and hence can't get the
         * compression to do the padding by pointlessly opening and
         * closing zlib blocks), then our other strategy is to precede
         * this message with an SSH_MSG_IGNORE that makes it up to the
         * right length.
         *
         * A third option in principle, and the most obviously
         * sensible, would be to set the explicit padding field in the
         * packet to more than its minimum value. Sadly, that turns
         * out to break some servers (our institutional memory thinks
         * Cisco in particular) and so we abandoned that idea shortly
         * after trying it.
         */

        /*
         * Calculate the length we expect the real packet to have.
         */
        int block, length;
        PktOut *ignore_pkt;

        block = s->out.cipher ? ssh2_cipher_alg(s->out.cipher)->blksize : 0;
        if (block < 8)
            block = 8;
        length = pkt->length;
        length += 4;       /* minimum 4 byte padding */
        length += block-1;
        length -= (length % block);
        if (s->out.mac)
            length += ssh2_mac_alg(s->out.mac)->len;

        if (length < pkt->minlen) {
            /*
             * We need an ignore message. Calculate its length.
             */
            length = pkt->minlen - length;

            /*
             * And work backwards from that to the length of the
             * contained string.
             */
            if (s->out.mac)
                length -= ssh2_mac_alg(s->out.mac)->len;
            length -= 8;               /* length field + min padding */
            length -= 5;               /* type code + string length prefix */

            if (length < 0)
                length = 0;

            ignore_pkt = ssh2_bpp_new_pktout(SSH2_MSG_IGNORE);
            put_uint32(ignore_pkt, length);
            while (length-- > 0)
                put_byte(ignore_pkt, random_byte());
            ssh2_bpp_format_packet_inner(s, ignore_pkt);
            bufchain_add(s->bpp.out_raw, ignore_pkt->data, ignore_pkt->length);
            ssh_free_pktout(ignore_pkt);
        }
    }

    ssh2_bpp_format_packet_inner(s, pkt);
    bufchain_add(s->bpp.out_raw, pkt->data, pkt->length);
}

static void ssh2_bpp_handle_output(BinaryPacketProtocol *bpp)
{
    struct ssh2_bpp_state *s = FROMFIELD(bpp, struct ssh2_bpp_state, bpp);
    PktOut *pkt;

    if (s->cbc_ignore_workaround) {
        /*
         * When using a CBC-mode cipher in SSH-2, it's necessary to
         * ensure that an attacker can't provide data to be encrypted
         * using an IV that they know. We ensure this by inserting an
         * SSH_MSG_IGNORE if the last cipher block of the previous
         * packet has already been sent to the network (which we
         * approximate conservatively by checking if it's vanished
         * from out_raw).
         */
        if (bufchain_size(s->bpp.out_raw) <
            (ssh2_cipher_alg(s->out.cipher)->blksize +
             ssh2_mac_alg(s->out.mac)->len)) {
            /*
             * There's less data in out_raw than the MAC size plus the
             * cipher block size, which means at least one byte of
             * that cipher block must already have left. Add an
             * IGNORE.
             */
            pkt = ssh_bpp_new_pktout(&s->bpp, SSH2_MSG_IGNORE);
            put_stringz(pkt, "");
            ssh2_bpp_format_packet(s, pkt);
        }
    }

    while ((pkt = pq_pop(&s->bpp.out_pq)) != NULL) {
        ssh2_bpp_format_packet(s, pkt);
        ssh_free_pktout(pkt);
    }
}
