/****************************************************************************
 * net/sixlowpan/sixlowpan_framelist.c
 *
 *   Copyright (C) 2017 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Parts of this file derive from Contiki:
 *
 *   Copyright (c) 2008, Swedish Institute of Computer Science.
 *   All rights reserved.
 *   Authors: Adam Dunkels <adam@sics.se>
 *            Nicolas Tsiftes <nvt@sics.se>
 *            Niclas Finne <nfi@sics.se>
 *            Mathilde Durvy <mdurvy@cisco.com>
 *            Julien Abeille <jabeille@cisco.com>
 *            Joakim Eriksson <joakime@sics.se>
 *            Joel Hoglund <joel@sics.se>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/net/netdev.h>

#include "sixlowpan/sixlowpan_internal.h"

#ifdef CONFIG_NET_6LOWPAN

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Configuration ************************************************************/

/* A single IOB must be big enough to hold a full frame */

#if CONFIG_IOB_BUFSIZE < CONFIG_NET_6LOWPAN_FRAMELEN
#  error IOBs must be large enough to hold full IEEE802.14.5 frame
#endif

/* There must be at least enough IOBs to hold the full MTU.  Probably still
 * won't work unless there are a few more.
 */

#if CONFIG_NET_6LOWPAN_MTU > (CONFIG_IOB_BUFSIZE * CONFIG_IOB_NBUFFERS)
#  error Not enough IOBs to hold one full 6LoWPAN packet
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sixlowpan_compress_ipv6hdr
 *
 * Description:
 *   IPv6 dispatch "compression" function.  Packets "Compression" when only
 *   IPv6 dispatch is used
 *
 *   There is no compression in this case, all fields are sent
 *   inline. We just add the IPv6 dispatch byte before the packet.
 *
 *   0               1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   | IPv6 Dsp      | IPv6 header and payload ...
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Input Parameters:
 *   ipv6hdr - Pointer to the IPv6 header to "compress"
 *   fptr    - Pointer to the beginning of the frame under construction
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void sixlowpan_compress_ipv6hdr(FAR const struct ipv6_hdr_s *ipv6hdr,
                                       FAR uint8_t *fptr)
{
  uint16_t protosize;

  /* Indicate the IPv6 dispatch and length */

  fptr[g_frame_hdrlen] = SIXLOWPAN_DISPATCH_IPV6;
  g_frame_hdrlen      += SIXLOWPAN_IPV6_HDR_LEN;

  /* Copy the IPv6 header and adjust pointers */

  memcpy(&fptr[g_frame_hdrlen] , ipv6hdr, IPv6_HDRLEN);
  g_frame_hdrlen      += IPv6_HDRLEN;
  g_uncomp_hdrlen     += IPv6_HDRLEN;

  /* Copy the following protocol header, */

   switch (ipv6hdr->proto)
     {
#ifdef CONFIG_NET_TCP
     case IP_PROTO_TCP:
       {
         FAR struct tcp_hdr_s *tcp = &((FAR struct ipv6tcp_hdr_s *)ipv6hdr)->tcp;

         /* The TCP header length is encoded in the top 4 bits of the
          * tcpoffset field (in units of 32-bit words).
          */

         protosize = ((uint16_t)tcp->tcpoffset >> 4) << 2;
       }
       break;
#endif

#ifdef CONFIG_NET_UDP
     case IP_PROTO_UDP:
       protosize = sizeof(struct udp_hdr_s);
       break;
#endif

#ifdef CONFIG_NET_ICMPv6
     case IP_PROTO_ICMP6:
       protosize = sizeof(struct icmpv6_hdr_s);
       break;
#endif

     default:
       nwarn("WARNING: Unrecognized proto: %u\n", ipv6hdr->proto);
       return;
     }

  /* Copy the protocol header. */

  memcpy(fptr + g_frame_hdrlen, (FAR uint8_t *)ipv6hdr + g_uncomp_hdrlen,
         protosize);

  g_frame_hdrlen  += protosize;
  g_uncomp_hdrlen += protosize;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sixlowpan_queue_frames
 *
 * Description:
 *   Process an outgoing UDP or TCP packet.  This function is called from
 *   send interrupt logic when a TX poll is received.  It formats the
 *   list of frames to be sent by the IEEE802.15.4 MAC driver.
 *
 *   The payload data is in the caller 'buf' and is of length 'buflen'.
 *   Compressed headers will be added and if necessary the packet is
 *   fragmented. The resulting packet/fragments are put in ieee->i_framelist
 *   and the entire list of frames will be delivered to the 802.15.4 MAC via
 *   ieee->i_framelist.
 *
 * Input Parameters:
 *   ieee    - The IEEE802.15.4 MAC driver instance
 *   ipv6hdr - IPv6 header followed by TCP, UDP, or ICMPv6 header.
 *   buf     - Beginning of the packet packet to send (with IPv6 + protocol
 *             headers)
 *   buflen  - Length of data to send (include IPv6 and protocol headers)
 *   destmac - The IEEE802.15.4 MAC address of the destination
 *
 * Returned Value:
 *   Ok is returned on success; Othewise a negated errno value is returned.
 *   This function is expected to fail if the driver is not an IEEE802.15.4
 *   MAC network driver.  In that case, the UDP/TCP will fall back to normal
 *   IPv4/IPv6 formatting.
 *
 * Assumptions:
 *   Called with the network locked.
 *
 ****************************************************************************/

int sixlowpan_queue_frames(FAR struct ieee802154_driver_s *ieee,
                           FAR const struct ipv6_hdr_s *destip,
                           FAR const void *buf, size_t buflen,
                           FAR const struct rimeaddr_s *destmac)
{
  FAR struct iob_s *iob;
  FAR uint8_t *fptr;
  int framer_hdrlen;
  struct rimeaddr_s bcastmac;
  uint16_t pktlen;
  uint16_t paysize;
  uint16_t dest_panid;
#ifdef CONFIG_NET_6LOWPAN_FRAG
  uint16_t outlen = 0;
#endif

  /* Initialize global data.  Locking the network guarantees that we have
   * exclusive use of the global values for intermediate calculations.
   */

  g_uncomp_hdrlen = 0;
  g_frame_hdrlen  = 0;

  /* Reset rime buffer, packet buffer metatadata */

  memset(g_pktattrs, 0, PACKETBUF_NUM_ATTRS * sizeof(uint16_t));
  memset(g_pktaddrs, 0, PACKETBUF_NUM_ADDRS * sizeof(struct rimeaddr_s));

  g_pktattrs[PACKETBUF_ATTR_MAX_MAC_TRANSMISSIONS] =
    CONFIG_NET_6LOWPAN_MAX_MACTRANSMITS;

  /* Set stream mode for all TCP packets, except FIN packets. */

  if (destip->proto == IP_PROTO_TCP)
    {
      FAR const struct tcp_hdr_s *tcp =
        &((FAR const struct ipv6tcp_hdr_s *)destip)->tcp;

      if ((tcp->flags & TCP_FIN) == 0 &&
          (tcp->flags & TCP_CTL) != TCP_ACK)
        {
          g_pktattrs[PACKETBUF_ATTR_PACKET_TYPE] = PACKETBUF_ATTR_PACKET_TYPE_STREAM;
        }
      else if ((tcp->flags & TCP_FIN) == TCP_FIN)
        {
          g_pktattrs[PACKETBUF_ATTR_PACKET_TYPE] = PACKETBUF_ATTR_PACKET_TYPE_STREAM_END;
        }
    }

  /* The destination address will be tagged to each outbound packet. If the
   * argument destmac is NULL, we are sending a broadcast packet.
   */

  if (destmac == NULL)
    {
      memset(&bcastmac, 0, sizeof(struct rimeaddr_s));
      destmac = &bcastmac;
    }

  /* Pre-allocate the IOB to hold frame or the first fragment, waiting if
   * necessary.
   */

  iob = iob_alloc(false);
  DEBUGASSERT(iob != NULL);

  /* Initialize the IOB */

  iob->io_flink  = NULL;
  iob->io_len    = 0;
  iob->io_offset = 0;
  iob->io_pktlen = 0;
  fptr           = iob->io_data;

  ninfo("Sending packet length %d\n", buflen);

  /* Set the source and destination address */

  rimeaddr_copy(&g_pktaddrs[PACKETBUF_ADDR_SENDER],
                &ieee->i_dev.d_mac.ieee802154);
  rimeaddr_copy(&g_pktaddrs[PACKETBUF_ADDR_RECEIVER], destmac);

  /* Get the destination PAN ID.
   *
   * REVISIT: For now I am assuming that the source and destination
   * PAN IDs are the same.
   */

  dest_panid = 0xffff;
  (void)sixlowpan_src_panid(ieee, &dest_panid);

  /* Pre-calculate frame header length. */

  framer_hdrlen = sixlowpan_send_hdrlen(ieee, dest_panid);
  if (framer_hdrlen < 0)
    {
      /* Failed to determine the size of the header failed. */

      nerr("ERROR: sixlowpan_send_hdrlen() failed: %d\n", framer_hdrlen);
      return framer_hdrlen;
    }

  g_frame_hdrlen  = framer_hdrlen;

#ifndef CONFIG_NET_6LOWPAN_COMPRESSION_IPv6
  if (buflen >= CONFIG_NET_6LOWPAN_COMPRESSION_THRESHOLD)
    {
      /* Try to compress the headers */

#if defined(CONFIG_NET_6LOWPAN_COMPRESSION_HC1)
      sixlowpan_compresshdr_hc1(ieee, destip, destmac, fptr);
#elif defined(CONFIG_NET_6LOWPAN_COMPRESSION_HC06)
      sixlowpan_compresshdr_hc06(ieee, destip, destmac, fptr);
#else
#  error No compression specified
#endif
    }
  else
#endif /* !CONFIG_NET_6LOWPAN_COMPRESSION_IPv6 */
    {
      /* Small.. use IPv6 dispatch (no compression) */

      sixlowpan_compress_ipv6hdr(destip, fptr);
    }

  ninfo("Header of length %d\n", g_frame_hdrlen);

  /* Check if we need to fragment the packet into several frames */

  if (buflen > (CONFIG_NET_6LOWPAN_FRAMELEN - g_frame_hdrlen))
    {
#ifdef CONFIG_NET_6LOWPAN_FRAG
      /* ieee->i_framelist will hold the generated frames; frames will be
       * added at qtail.
       */

      FAR struct iob_s *qtail;
      FAR uint8_t *frame1;
      FAR uint8_t *fragptr;
      uint16_t frag1_hdrlen;
      int verify;

      /* The outbound IPv6 packet is too large to fit into a single 15.4
       * packet, so we fragment it into multiple packets and send them.
       * The first fragment contains frag1 dispatch, then
       * IPv6/HC1/HC06/HC_UDP dispatchs/headers.
       * The following fragments contain only the fragn dispatch.
       */

      ninfo("Sending fragmented packet length %d\n", buflen);

      /* Create 1st Fragment */
      /* Add the frame header using the pre-allocated IOB using the DSN
       * selected by sixlowpan_send_hdrlen().
       */

      verify = sixlowpan_framecreate(ieee, iob, dest_panid);
      DEBUGASSERT(verify == framer_hdrlen);
      UNUSED(verify);

      /* Move HC1/HC06/IPv6 header to make space for the FRAG1 header at the
       * beginning of the frame.
       */

      fragptr = fptr + framer_hdrlen;
      memmove(fragptr + SIXLOWPAN_FRAG1_HDR_LEN, fragptr,
              g_frame_hdrlen - framer_hdrlen);

      /* Setup up the fragment header.
       *
       * The fragment header contains three fields:  Datagram size, datagram
       * tag and datagram offset:
       *
       * 1. Datagram size describes the total (un-fragmented) payload.
       * 2. Datagram tag identifies the set of fragments and is used to
       *    match fragments of the same payload.
       * 3. Datagram offset identifies the fragment’s offset within the un-
       *    fragmented payload.
       *
       * The fragment header length is 4 bytes for the first header and 5
       * bytes for all subsequent headers.
       */

      pktlen = buflen + g_uncomp_hdrlen;
      PUTINT16(fragptr, RIME_FRAG_DISPATCH_SIZE,
               ((SIXLOWPAN_DISPATCH_FRAG1 << 8) | pktlen));
      PUTINT16(fragptr, RIME_FRAG_TAG, ieee->i_dgramtag);

      g_frame_hdrlen += SIXLOWPAN_FRAG1_HDR_LEN;

      /* Copy payload and enqueue.  NOTE that the size is a multiple of eight
       * bytes.
       */

      paysize = (CONFIG_NET_6LOWPAN_FRAMELEN - g_frame_hdrlen) & ~7;
      memcpy(fptr + g_frame_hdrlen, buf,  paysize);

      /* Set outlen to what we already sent from the IP payload */

      iob->io_len       = paysize + g_frame_hdrlen;
      outlen            = paysize;

      ninfo("First fragment: length %d, tag %d\n",
            paysize, ieee->i_dgramtag);
      sixlowpan_dumpbuffer("Outgoing frame",
                           (FAR const uint8_t *)iob->io_data, iob->io_len);

      /* Add the first frame to the IOB queue */

      ieee->i_framelist = iob;
      qtail             = iob;

      /* Keep track of the total amount of data queue */

      iob->io_pktlen    = iob->io_len;

      /* Create following fragments */

      frame1            = iob->io_data;
      frag1_hdrlen      = g_frame_hdrlen;

      while (outlen < buflen)
        {
          uint16_t fragn_hdrlen;

          /* Allocate an IOB to hold the next fragment, waiting if
           * necessary.
           */

          iob = iob_alloc(false);
          DEBUGASSERT(iob != NULL);

          /* Initialize the IOB */

          iob->io_flink  = NULL;
          iob->io_len    = 0;
          iob->io_offset = 0;
          iob->io_pktlen = 0;
          fptr           = iob->io_data;

          /* Add a new frame header to the IOB (same as the first but with a
           * different DSN).
           */

          g_pktattrs[PACKETBUF_ATTR_MAC_SEQNO] = 0;

          verify = sixlowpan_framecreate(ieee, iob, ieee->i_panid);
          DEBUGASSERT(verify == framer_hdrlen);
          UNUSED(verify);

          /* Copy the HC1/HC06/IPv6 header the frame header from first
           * frame, into the correct location after the FRAGN header
           * of subsequent frames.
           */

          fragptr = fptr + framer_hdrlen;
          memcpy(fragptr + SIXLOWPAN_FRAGN_HDR_LEN,
                 frame1 + framer_hdrlen + SIXLOWPAN_FRAG1_HDR_LEN,
                 frag1_hdrlen - framer_hdrlen);
          fragn_hdrlen = frag1_hdrlen - SIXLOWPAN_FRAG1_HDR_LEN;

          /* Setup up the FRAGN header after the frame header. */

          PUTINT16(fragptr, RIME_FRAG_DISPATCH_SIZE,
                   ((SIXLOWPAN_DISPATCH_FRAGN << 8) | pktlen));
          PUTINT16(fragptr, RIME_FRAG_TAG, ieee->i_dgramtag);
          fragptr[RIME_FRAG_OFFSET] = outlen >> 3;

          fragn_hdrlen += SIXLOWPAN_FRAGN_HDR_LEN;

          /* Copy payload and enqueue */
          /* Check for the last fragment */

          paysize = (CONFIG_NET_6LOWPAN_FRAMELEN - fragn_hdrlen) &
                    SIXLOWPAN_DISPATCH_FRAG_MASK;
          if (buflen - outlen < paysize)
            {
              /* Last fragment, truncate to the correct length */

              paysize = buflen - outlen;
            }

          memcpy(fptr + fragn_hdrlen, buf + outlen, paysize);

          /* Set outlen to what we already sent from the IP payload */

          iob->io_len = paysize + fragn_hdrlen;
          outlen     += paysize;

          ninfo("Fragment offset=%d, paysize=%d, i_dgramtag=%d\n",
                outlen >> 3, paysize, ieee->i_dgramtag);
          sixlowpan_dumpbuffer("Outgoing frame",
                               (FAR const uint8_t *)iob->io_data,
                               iob->io_len);

          /* Add the next frame to the tail of the IOB queue */

          qtail->io_flink = iob;
          qtail           = iob;

          /* Keep track of the total amount of data queue */

          ieee->i_framelist->io_pktlen += iob->io_len;
        }

      /* Update the datagram TAG value */

      ieee->i_dgramtag++;
#else
      nerr("ERROR: Packet too large: %d\n", buflen);
      nerr("       Cannot to be sent without fragmentation support\n");
      nerr("       dropping packet\n");

      return -E2BIG;
#endif
    }
  else
    {
      int verify;

      /* The packet does not need to be fragmented just copy the "payload"
       * and send in one frame.
       */

      /* Add the frame header to the preallocated IOB. */

      verify = sixlowpan_framecreate(ieee, iob, dest_panid);
      DEBUGASSERT(verify == framer_hdrlen);
      UNUSED(verify);

      /* Copy the payload and queue */

      memcpy(fptr + g_frame_hdrlen, buf, buflen);
      iob->io_len = buflen + g_frame_hdrlen;

      ninfo("Non-fragmented: length %d\n", iob->io_len);
      sixlowpan_dumpbuffer("Outgoing frame",
                       (FAR const uint8_t *)iob->io_data, iob->io_len);

      /* Add the first frame to the IOB queue */

      ieee->i_framelist = iob;

      /* Keep track of the total amount of data queue */

      iob->io_pktlen    = iob->io_len;
    }

  return OK;
}

#endif /* CONFIG_NET_6LOWPAN */
