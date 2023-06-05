/*
 *  Copyright (c) 2023 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_RTP_RTCP_SOURCE_FLEXFEC_HEADER_READER_WRITER2_H_
#define MODULES_RTP_RTCP_SOURCE_FLEXFEC_HEADER_READER_WRITER2_H_

#include <stddef.h>
#include <stdint.h>

#include "modules/rtp_rtcp/source/forward_error_correction.h"

namespace webrtc {

// FlexFEC header in flexible mode (R=0, F=0), minimum 12 bytes.
// https://datatracker.ietf.org/doc/html/rfc8627#section-4.2.2.1
//
//     0                   1                   2                   3
//     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  0 |0|0|P|X|  CC   |M| PT recovery |        length recovery        |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  4 |                          TS recovery                          |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  8 |           SN base_i           |k|          Mask [0-14]        |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// 12 |k|                   Mask [15-45] (optional)                   |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// 16 |                     Mask [46-109] (optional)                  |
// 20 |                                                               |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |   ... next SN base and Mask for CSRC_i in CSRC list ...       |
//

class FlexfecHeaderReader2 : public FecHeaderReader {
 public:
  FlexfecHeaderReader2();
  ~FlexfecHeaderReader2() override;

  bool ReadFecHeader(
      ForwardErrorCorrection::ReceivedFecPacket* fec_packet) const override;
};

class FlexfecHeaderWriter2 : public FecHeaderWriter {
 public:
  FlexfecHeaderWriter2();
  ~FlexfecHeaderWriter2() override;

  size_t MinPacketMaskSize(const uint8_t* packet_mask,
                           size_t packet_mask_size) const override;

  size_t FecHeaderSize(size_t packet_mask_row_size) const override;

  void FinalizeFecHeader(
      uint32_t media_ssrc,
      uint16_t seq_num_base,
      const uint8_t* packet_mask,
      size_t packet_mask_size,
      ForwardErrorCorrection::Packet* fec_packet) const override;
};

}  // namespace webrtc

#endif  // MODULES_RTP_RTCP_SOURCE_FLEXFEC_HEADER_READER_WRITER2_H_
